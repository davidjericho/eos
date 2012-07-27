// -----------------------------------------------------------------------------
// File: RaidDpFile.cc
// Author: Elvin-Alin Sindrilaru - CERN
// -----------------------------------------------------------------------------

/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2011 CERN/Switzerland                                  *
 *                                                                      *
 * This program is free software: you can redistribute it and/or modify *
 * it under the terms of the GNU General Public License as published by *
 * the Free Software Foundation, either version 3 of the License, or    *
 * (at your option) any later version.                                  *
 *                                                                      *
 * This program is distributed in the hope that it will be useful,      *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of       *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        *
 * GNU General Public License for more details.                         *
 *                                                                      *
 * You should have received a copy of the GNU General Public License    *
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.*
 ************************************************************************/

/*----------------------------------------------------------------------------*/
#include <set>
#include <map>
#include <cassert>
#include <cmath>
#include <fcntl.h>
/*----------------------------------------------------------------------------*/
#include "common/Timing.hh"
#include "fst/io/RaidDpFile.hh"
/*----------------------------------------------------------------------------*/

EOSFSTNAMESPACE_BEGIN

typedef long v2do __attribute__( ( vector_size( VECTOR_SIZE ) ) );

// -----------------------------------------------------------------------------
// Constructor
// -----------------------------------------------------------------------------
RaidDpFile::RaidDpFile( std::vector<std::string> stripeUrl,
                        int                      numParity,
                        bool                     storeRecovery,
                        bool                     isStreaming,
                        off_t                    targetSize,
                        std::string              bookingOpaque )
  : RaidIO( "raidDP", stripeUrl, numParity, storeRecovery,
            isStreaming, targetSize, bookingOpaque )
{
  assert( mNbParityFiles = 2 );
  mNbDataBlocks = static_cast<int>( pow( mNbDataFiles, 2 ) );
  mNbTotalBlocks = mNbDataBlocks + 2 * mNbDataFiles;
  mSizeGroup = mNbDataBlocks * mStripeWidth;

  //............................................................................
  // Allocate memory for blocks
  //............................................................................
  for ( unsigned int i = 0; i < mNbTotalBlocks; i++ ) {
    mDataBlocks.push_back( new char[mStripeWidth] );
  }
}


// -----------------------------------------------------------------------------
// Destructor
// -----------------------------------------------------------------------------
RaidDpFile::~RaidDpFile()
{
  for ( unsigned int i = 0; i < mNbTotalBlocks; i++ ) {
    delete[] mDataBlocks[i];
  }
}


// -----------------------------------------------------------------------------
// Compute simple and double parity blocks
// -----------------------------------------------------------------------------
void
RaidDpFile::ComputeParity()
{
  int index_pblock;
  int current_block;

  //............................................................................
  // Compute simple parity
  //............................................................................
  for ( unsigned int i = 0; i < mNbDataFiles; i++ ) {
    index_pblock = ( i + 1 ) * mNbDataFiles + 2 * i;
    current_block = i * ( mNbDataFiles + 2 );   //beginning of current line
    OperationXOR( mDataBlocks[current_block],
                  mDataBlocks[current_block + 1],
                  mDataBlocks[index_pblock],
                  mStripeWidth );
    current_block += 2;

    while ( current_block < index_pblock ) {
      OperationXOR( mDataBlocks[index_pblock],
                    mDataBlocks[current_block],
                    mDataBlocks[index_pblock],
                    mStripeWidth );
      current_block++;
    }
  }

  //............................................................................
  // Compute double parity
  //............................................................................
  unsigned int aux_block;
  unsigned int next_block;
  unsigned int index_dpblock;
  unsigned int jump_blocks = mNbTotalFiles + 1;
  vector<int> used_blocks;

  for ( unsigned int i = 0; i < mNbDataFiles; i++ ) {
    index_dpblock = ( i + 1 ) * ( mNbDataFiles + 1 ) +  i;
    used_blocks.push_back( index_dpblock );
  }

  for ( unsigned int i = 0; i < mNbDataFiles; i++ ) {
    index_dpblock = ( i + 1 ) * ( mNbDataFiles + 1 ) +  i;
    next_block = i + jump_blocks;
    OperationXOR( mDataBlocks[i],
                  mDataBlocks[next_block],
                  mDataBlocks[index_dpblock],
                  mStripeWidth );
    used_blocks.push_back( i );
    used_blocks.push_back( next_block );

    for ( unsigned int j = 0; j < mNbDataFiles - 2; j++ ) {
      aux_block = next_block + jump_blocks;

      if ( ( aux_block < mNbTotalBlocks ) &&
           ( find( used_blocks.begin(), used_blocks.end(), aux_block ) == used_blocks.end() ) ) {
        next_block = aux_block;
      } else {
        next_block++;

        while ( find( used_blocks.begin(), used_blocks.end(), next_block ) != used_blocks.end() ) {
          next_block++;
        }
      }

      OperationXOR( mDataBlocks[index_dpblock],
                    mDataBlocks[next_block],
                    mDataBlocks[index_dpblock],
                    mStripeWidth );
      used_blocks.push_back( next_block );
    }
  }
}


// -----------------------------------------------------------------------------
// XOR the two blocks using 128 bits and return the result
// -----------------------------------------------------------------------------
void
RaidDpFile::OperationXOR( char*  pBlock1,
                          char*  pBlock2,
                          char*  pResult,
                          size_t totalBytes )
{
  v2do* xor_res;
  v2do* idx1;
  v2do* idx2;
  char* byte_res;
  char* byte_idx1;
  char* byte_idx2;
  long int noPices = -1;
  idx1 = ( v2do* ) pBlock1;
  idx2 = ( v2do* ) pBlock2;
  xor_res = ( v2do* ) pResult;
  noPices = totalBytes / sizeof( v2do );

  for ( unsigned int i = 0; i < noPices; idx1++, idx2++, xor_res++, i++ ) {
    *xor_res = *idx1 ^ *idx2;
  }

  //............................................................................
  // If the block does not devide perfectly to 128 ...
  //............................................................................
  if ( totalBytes % sizeof( v2do ) != 0 ) {
    byte_res = ( char* ) xor_res;
    byte_idx1 = ( char* ) idx1;
    byte_idx2 = ( char* ) idx2;

    for ( unsigned int i = noPices * sizeof( v2do );
          i < totalBytes;
          byte_res++, byte_idx1++, byte_idx2++, i++ ) {
      *byte_res = *byte_idx1 ^ *byte_idx2;
    }
  }
}


// -----------------------------------------------------------------------------
// Try to recover the block at the current offset
// -----------------------------------------------------------------------------
bool
RaidDpFile::RecoverPieces( off_t                    offsetInit,
                           char*                    pBuffer,
                           std::map<off_t, size_t>& rMapToRecover )
{
  //............................................................................
  // Obs: DoubleParityRecover also checks the simple and double parity blocks
  //............................................................................
  mDoneRecovery = DoubleParityRecover( offsetInit, pBuffer, rMapToRecover );
  return mDoneRecovery;
}


// -----------------------------------------------------------------------------
// Use simple and double parity to recover corrupted pieces
// -----------------------------------------------------------------------------
bool
RaidDpFile::DoubleParityRecover( off_t                    offsetInit,
                                 char*                    pBuffer,
                                 std::map<off_t, size_t>& rMapToRecover )
{
  bool* status_blocks;
  char* pBuff;
  size_t length;
  off_t offset_local;
  unsigned int id_stripe;
  vector<int> corrupt_ids;
  vector<int> exclude_ids;
  off_t offset = rMapToRecover.begin()->first;
  off_t offset_group = ( offset / mSizeGroup ) * mSizeGroup;
  std::map<uint64_t, uint32_t> mapErrors;
  vector<unsigned int> simple_parity = GetSimpleParityIndices();
  vector<unsigned int> double_parity = GetDoubleParityIndices();
  status_blocks = static_cast<bool*>( calloc( mNbTotalBlocks, sizeof( bool ) ) );

  //............................................................................
  // Reset the read and write handlers
  //............................................................................
  for ( unsigned int i = 0; i < mNbTotalFiles; i++ ) {
    mReadHandlers[i]->Reset();
    mWriteHandlers[i]->Reset();
  }

  for ( unsigned int i = 0; i < mNbTotalBlocks; i++ ) {
    memset( mDataBlocks[i], 0, mStripeWidth );
    status_blocks[i] = true;
    id_stripe = i % mNbTotalFiles;
    offset_local = ( offset_group / ( mNbDataFiles * mStripeWidth ) ) *  mStripeWidth +
                   ( ( i / mNbTotalFiles ) * mStripeWidth );
    mReadHandlers[id_stripe]->Increment();
    mFiles[mapSU[id_stripe]]->Read( offset_local + mSizeHeader, mStripeWidth,
                                       mDataBlocks[i], mReadHandlers[id_stripe] );
  }

  //............................................................................
  // Mark the corrupted blocks
  //............................................................................
  for ( unsigned int i = 0; i < mNbTotalFiles; i++ ) {
    if ( !mReadHandlers[i]->WaitOK() ) {
      mapErrors = mReadHandlers[i]->GetErrorsMap();

      for ( std::map<uint64_t, uint32_t>::iterator iter = mapErrors.begin();
            iter != mapErrors.end();
            iter++ ) {
        off_t off_stripe = iter->first - mSizeHeader;
        int index_stripe = ( off_stripe % ( mNbDataFiles * mStripeWidth ) ) / mStripeWidth;
        int index = index_stripe * mNbTotalFiles + i;
        status_blocks[index] = false;
        corrupt_ids.push_back( index );
      }
    }
  }

  //............................................................................
  // Recovery algorithm
  //............................................................................
  unsigned int id_corrupted;
  vector<unsigned int> horizontal_stripe;
  vector<unsigned int> diagonal_stripe;

  while ( !corrupt_ids.empty() ) {
    id_corrupted = corrupt_ids.back();
    corrupt_ids.pop_back();

    if ( ValidHorizStripe( horizontal_stripe, status_blocks, id_corrupted ) ) {
      //........................................................................
      // Try to recover using simple parity
      //........................................................................
      memset( mDataBlocks[id_corrupted], 0, mStripeWidth );

      for ( unsigned int ind = 0;  ind < horizontal_stripe.size(); ind++ ) {
        if ( horizontal_stripe[ind] != id_corrupted ) {
          OperationXOR( mDataBlocks[id_corrupted],
                        mDataBlocks[horizontal_stripe[ind]],
                        mDataBlocks[id_corrupted],
                        mStripeWidth );
        }
      }

      //........................................................................
      // Return recovered block and also write it to the file
      //........................................................................
      id_stripe = id_corrupted % mNbTotalFiles;
      offset_local = ( ( offset_group / ( mNbDataFiles * mStripeWidth ) ) * mStripeWidth ) +
                     ( ( id_corrupted / mNbTotalFiles ) * mStripeWidth );

      if ( mStoreRecovery ) {
        mWriteHandlers[id_stripe]->Increment();
        mFiles[mapSU[id_stripe]]->Write( offset_local + mSizeHeader,
                                            mStripeWidth,
                                            mDataBlocks[id_corrupted],
                                            mWriteHandlers[id_stripe] );
      }

      for ( std::map<off_t, size_t>::iterator iter = rMapToRecover.begin();
            iter != rMapToRecover.end();
            iter++ ) {
        offset = iter->first;
        length = iter->second;

        //......................................................................
        // If not SP or DP, maybe we have to return it
        //......................................................................
        if ( find( simple_parity.begin(), simple_parity.end(), id_corrupted ) == simple_parity.end() &&
             find( double_parity.begin(), double_parity.end(), id_corrupted ) == double_parity.end() ) {
          if ( ( offset >= ( off_t )( offset_group + MapBigToSmall( id_corrupted ) * mStripeWidth ) ) &&
               ( offset < ( off_t )( offset_group + ( MapBigToSmall( id_corrupted ) + 1 ) * mStripeWidth ) ) ) {
            pBuff = pBuffer + ( offset - offsetInit );
            pBuff = static_cast<char*>( memcpy( pBuff, mDataBlocks[id_corrupted] +
                                                ( offset % mStripeWidth ), length ) );
          }
        }
      }

      //........................................................................
      // Copy the unrecoverd blocks back in the queue
      //........................................................................
      if ( !exclude_ids.empty() ) {
        corrupt_ids.insert( corrupt_ids.end(), exclude_ids.begin(), exclude_ids.end() );
        exclude_ids.clear();
      }

      status_blocks[id_corrupted] = true;
    } else {
      //........................................................................
      // Try to recover using double parity
      //........................................................................
      if ( ValidDiagStripe( diagonal_stripe, status_blocks, id_corrupted ) ) {
        memset( mDataBlocks[id_corrupted], 0, mStripeWidth );

        for ( unsigned int ind = 0;  ind < diagonal_stripe.size(); ind++ ) {
          if ( diagonal_stripe[ind] != id_corrupted ) {
            OperationXOR( mDataBlocks[id_corrupted],
                          mDataBlocks[diagonal_stripe[ind]],
                          mDataBlocks[id_corrupted],
                          mStripeWidth );
          }
        }

        //......................................................................
        // Return recovered block and also write it to the file
        //......................................................................
        id_stripe = id_corrupted % mNbTotalFiles;
        offset_local = ( ( offset_group / ( mNbDataFiles * mStripeWidth ) ) * mStripeWidth ) +
                       ( ( id_corrupted / mNbTotalFiles ) * mStripeWidth );

        if ( mStoreRecovery ) {
          mWriteHandlers[id_stripe]->Increment();
          mFiles[mapSU[id_stripe]]->Write( offset_local + mSizeHeader, mStripeWidth,
                                              mDataBlocks[id_corrupted], mWriteHandlers[id_stripe] );
        }

        for ( std::map<off_t, size_t>::iterator iter = rMapToRecover.begin();
              iter != rMapToRecover.end();
              iter++ ) {
          offset = iter->first;
          length = iter->second;

          //....................................................................
          // If not SP or DP, maybe we have to return it
          //....................................................................
          if ( find( simple_parity.begin(), simple_parity.end(), id_corrupted ) == simple_parity.end() &&
               find( double_parity.begin(), double_parity.end(), id_corrupted ) == double_parity.end() ) {
            if ( ( offset >= ( off_t )( offset_group + MapBigToSmall( id_corrupted ) * mStripeWidth ) ) &&
                 ( offset < ( off_t )( offset_group + ( MapBigToSmall( id_corrupted ) + 1 ) * mStripeWidth ) ) ) {
              pBuff = pBuffer + ( offset - offsetInit );
              pBuff = static_cast<char*>( memcpy( pBuff, mDataBlocks[id_corrupted] +
                                                  ( offset % mStripeWidth ), length ) );
            }
          }
        }

        //......................................................................
        // Copy the unrecoverd blocks back in the queue
        //......................................................................
        if ( !exclude_ids.empty() ) {
          corrupt_ids.insert( corrupt_ids.end(), exclude_ids.begin(), exclude_ids.end() );
          exclude_ids.clear();
        }

        status_blocks[id_corrupted] = true;
      } else {
        //......................................................................
        // Current block can not be recoverd in this configuration
        //......................................................................
        exclude_ids.push_back( id_corrupted );
      }
    }
  }
  
  //............................................................................
  // Wait for write responses and reset all handlers
  //............................................................................
  for ( unsigned int i = 0; i < mNbTotalFiles; i++ ) {
    if ( !mWriteHandlers[i]->WaitOK() ) {
      free( status_blocks );
      eos_err( "Write stripe %s- write failed", mStripeUrls[mapSU[i]].c_str() );
      return false;
    }

    mWriteHandlers[i]->Reset();
    mReadHandlers[i]->Reset();
  }

  free( status_blocks );

  if ( corrupt_ids.empty() && !exclude_ids.empty() ) {
    return false;
  }

  return true;
}


// -----------------------------------------------------------------------------
// Add a new data used to compute parity block
// -----------------------------------------------------------------------------
void
RaidDpFile::AddDataBlock( off_t offset, char* buffer, size_t length )
{
  int indx_block;
  size_t nwrite;
  off_t offset_in_block;
  off_t offset_in_group = offset % mSizeGroup;

  if ( ( mOffGroupParity == -1 ) && ( offset < static_cast<off_t>( mSizeGroup ) ) ) {
    mOffGroupParity = 0;
  }

  if ( offset_in_group == 0 ) {
    mFullDataBlocks = false;

    for ( unsigned int i = 0; i < mNbTotalBlocks; i++ ) {
      memset( mDataBlocks[i], 0, mStripeWidth );
    }
  }

  char* ptr;
  size_t available_length;

  while ( length ) {
    offset_in_block = offset_in_group % mStripeWidth;
    available_length = mStripeWidth - offset_in_block;
    indx_block = MapSmallToBig( offset_in_group / mStripeWidth );
    nwrite = ( length > available_length ) ? available_length : length;
    ptr = mDataBlocks[indx_block];
    ptr += offset_in_block;
    ptr = static_cast<char*>( memcpy( ptr, buffer, nwrite ) );
    offset += nwrite;
    length -= nwrite;
    buffer += nwrite;
    offset_in_group = offset % mSizeGroup;

    if ( offset_in_group == 0 ) {
      //........................................................................
      // We completed a group, we can compute parity
      //........................................................................
      mOffGroupParity = ( ( offset - 1 ) / mSizeGroup ) *  mSizeGroup;
      mFullDataBlocks = true;
      DoBlockParity( mOffGroupParity );
      mOffGroupParity += mSizeGroup;

      for ( unsigned int i = 0; i < mNbTotalBlocks; i++ ) {
        memset( mDataBlocks[i], 0, mStripeWidth );
      }
    }
  }
}

// -----------------------------------------------------------------------------
// Write the parity blocks from mDataBlocks to the corresponding file stripes
// -----------------------------------------------------------------------------
int
RaidDpFile::WriteParityToFiles( off_t offsetGroup )
{
  unsigned int id_pfile;
  unsigned int id_dpfile;
  unsigned int index_pblock;
  unsigned int index_dpblock;
  off_t off_parity_local;
  id_pfile = mNbTotalFiles - 2;
  id_dpfile = mNbTotalFiles - 1;
  mWriteHandlers[id_pfile]->Reset();
  mWriteHandlers[id_dpfile]->Reset();

  for ( unsigned int i = 0; i < mNbDataFiles; i++ ) {
    index_pblock = ( i + 1 ) * mNbDataFiles + 2 * i;
    index_dpblock = ( i + 1 ) * ( mNbDataFiles + 1 ) +  i;
    off_parity_local = ( offsetGroup / mNbDataFiles ) + ( i * mStripeWidth );

    //..........................................................................
    // Writing simple parity
    //..........................................................................
    mWriteHandlers[id_pfile]->Increment();
    mFiles[mapSU[id_pfile]]->Write( off_parity_local + mSizeHeader, mStripeWidth,
                                       mDataBlocks[index_pblock], mWriteHandlers[id_pfile] );

    //..........................................................................
    // Writing double parity
    //..........................................................................
    mWriteHandlers[id_dpfile]->Increment();
    mFiles[mapSU[id_dpfile]]->Write( off_parity_local + mSizeHeader, mStripeWidth,
                                        mDataBlocks[index_dpblock], mWriteHandlers[id_dpfile] );
  }

  if ( !mWriteHandlers[id_pfile]->WaitOK() || !mWriteHandlers[id_dpfile]->WaitOK() ) {
    eos_err( "error=error while writing parity information" );
    return -1;
  }

  return SFS_OK;
}


// -----------------------------------------------------------------------------
// Return the indices of the simple parity blocks from a group
// -----------------------------------------------------------------------------
vector<unsigned int>
RaidDpFile::GetSimpleParityIndices()
{
  unsigned int val = mNbDataFiles;
  vector<unsigned int> values;
  values.push_back( val );
  val++;

  for ( unsigned int i = 1; i < mNbDataFiles; i++ ) {
    val += ( mNbDataFiles + 1 );
    values.push_back( val );
    val++;
  }

  return values;
}


// -----------------------------------------------------------------------------
// Return the indices of the double parity blocks from a group
// -----------------------------------------------------------------------------
vector<unsigned int>
RaidDpFile::GetDoubleParityIndices()
{
  unsigned int val = mNbDataFiles;
  vector<unsigned int> values;
  val++;
  values.push_back( val );

  for ( unsigned int i = 1; i < mNbDataFiles; i++ ) {
    val += ( mNbDataFiles + 1 );
    val++;
    values.push_back( val );
  }

  return values;
}


// -----------------------------------------------------------------------------
// Check if the diagonal stripe is valid in the sense that there is at most one
// corrupted block in the current stripe and this is not the ommited diagonal
// -----------------------------------------------------------------------------
bool
RaidDpFile::ValidDiagStripe( std::vector<unsigned int>& rStripes,
                             bool*                      pStatusBlocks,
                             unsigned int               blockId )
{
  int corrupted = 0;
  rStripes.clear();
  rStripes = GetDiagonalStripe( blockId );

  if ( rStripes.size() == 0 ) return false;
  //............................................................................
  // The ommited diagonal contains the block with index mNbDataFilesBlocks
  //............................................................................
  if ( find( rStripes.begin(), rStripes.end(), mNbDataFiles ) != rStripes.end() )
    return false;

  for ( std::vector<unsigned int>::iterator iter = rStripes.begin();
        iter != rStripes.end();
        ++iter ) {
    if ( pStatusBlocks[*iter] == false ) {
      corrupted++;
    }

    if ( corrupted >= 2 ) {
      return false;
    }
  }

  return true;
}


// -----------------------------------------------------------------------------
// Check if the HORIZONTAL stripe is valid in the sense that there is at
// most one corrupted block in the current stripe
// -----------------------------------------------------------------------------
bool
RaidDpFile::ValidHorizStripe( std::vector<unsigned int>& rStripes,
                              bool*                      pStatusBlock,
                              unsigned int               blockId )
{
  int corrupted = 0;
  long int base_id = ( blockId / mNbTotalFiles ) * mNbTotalFiles;
  rStripes.clear();

  //............................................................................
  // If double parity block then no horizontal stripes
  //............................................................................
  if ( blockId == ( base_id + mNbDataFiles + 1 ) )
    return false;

  for ( unsigned int i = 0; i < mNbTotalFiles - 1; i++ )
    rStripes.push_back( base_id + i );

  //............................................................................
  // Check if it is valid
  //............................................................................
  for ( std::vector<unsigned int>::iterator iter = rStripes.begin();
        iter != rStripes.end();
        ++iter ) {
    if ( pStatusBlock[*iter] == false ) {
      corrupted++;
    }

    if ( corrupted >= 2 ) {
      return false;
    }
  }

  return true;
}


// -----------------------------------------------------------------------------
// Return the blocks corrsponding to the diagonal stripe of blockId
// -----------------------------------------------------------------------------
std::vector<unsigned int>
RaidDpFile::GetDiagonalStripe( unsigned int blockId )
{
  bool dp_added = false;
  std::vector<unsigned int> last_column = GetDoubleParityIndices();
  unsigned int next_block;
  unsigned int jump_blocks;
  unsigned int idLastBlock;
  unsigned int previous_block;
  std::vector<unsigned int> stripe;

  //............................................................................
  // If we are on the ommited diagonal, return
  //............................................................................
  if ( blockId == mNbDataFiles ) {
    stripe.clear();
    return stripe;
  }

  stripe.push_back( blockId );

  //............................................................................
  // If we start with a dp index, then construct the diagonal in a special way
  //............................................................................
  if ( find( last_column.begin(), last_column.end(), blockId ) != last_column.end() ) {
    blockId = blockId % ( mNbDataFiles + 1 );
    stripe.push_back( blockId );
    dp_added = true;
  }

  previous_block = blockId;
  jump_blocks = mNbDataFiles + 3;
  idLastBlock = mNbTotalBlocks - 1;

  for ( unsigned int i = 0 ; i < mNbDataFiles - 1; i++ ) {
    next_block = previous_block + jump_blocks;

    if ( next_block > idLastBlock ) {
      next_block %= idLastBlock;

      if ( next_block >= mNbDataFiles + 1 ) {
        next_block = ( previous_block + jump_blocks ) % jump_blocks;
      }
    } else if ( find( last_column.begin(), last_column.end(), next_block ) != last_column.end() ) {
      next_block = previous_block + 2;
    }

    stripe.push_back( next_block );
    previous_block = next_block;

    //..........................................................................
    // If on the ommited diagonal return
    //..........................................................................
    if ( next_block == mNbDataFiles ) {
      eos_debug( "Return empty vector - ommited diagonal" );
      stripe.clear();
      return stripe;
    }
  }
  
  //............................................................................
  // Add the index from the double parity block
  //............................................................................
  if ( !dp_added ) {
    next_block = GetDParityBlock( stripe );
    stripe.push_back( next_block );
  }

  return stripe;
}


// -----------------------------------------------------------------------------
// Return the id of stripe from a mNbTotalBlocks representation to a mNbDataBlocks
// representation in which we exclude the parity and double parity blocks
// -----------------------------------------------------------------------------
unsigned int
RaidDpFile::MapBigToSmall( unsigned int idBig )
{
  if ( ( idBig % ( mNbDataFiles + 2 ) == mNbDataFiles )  ||
       ( idBig % ( mNbDataFiles + 2 ) == mNbDataFiles + 1 ) )
    return -1;
  else
    return ( ( idBig / ( mNbDataFiles + 2 ) ) * mNbDataFiles +
             ( idBig % ( mNbDataFiles + 2 ) ) );
}


// -----------------------------------------------------------------------------
// Return the id of stripe from a mNbDataBlocks representation in a mNbTotalBlocks
// representation
// -----------------------------------------------------------------------------
unsigned int
RaidDpFile::MapSmallToBig( unsigned int idSmall )
{
  if ( idSmall >= mNbDataBlocks ) {
    eos_err( "error=idSmall bugger than expected" );
    return -1;
  }

  return ( idSmall / mNbDataFiles ) * ( mNbDataFiles + 2 ) + idSmall % mNbDataFiles;
}


// -----------------------------------------------------------------------------
// Return the id (out of mNbTotalBlocks) for the parity block corresponding to
// the current block
// -----------------------------------------------------------------------------
unsigned int
RaidDpFile::GetSParityBlock( unsigned int elemFromStripe )
{
  return ( mNbDataFiles + ( elemFromStripe / ( mNbDataFiles + 2 ) )
           * ( mNbDataFiles + 2 ) );
}


// -----------------------------------------------------------------------------
// Return the id (out of mNbTotalBlocks) for the double parity block corresponding
// to the current block
// -----------------------------------------------------------------------------
unsigned int
RaidDpFile::GetDParityBlock( std::vector<unsigned int>& rStripe )
{
  int min = *( std::min_element( rStripe.begin(), rStripe.end() ) );
  return ( ( min + 1 ) * ( mNbDataFiles + 1 ) + min );
}


// -----------------------------------------------------------------------------
// Truncate file
// -----------------------------------------------------------------------------
int
RaidDpFile::truncate( off_t offset )
{
  int rc = SFS_OK;
  off_t truncate_offset = 0;

  if ( !offset ) return rc;

  truncate_offset = ceil( ( offset * 1.0 ) / mSizeGroup ) * mStripeWidth * mNbDataFiles;
  truncate_offset += mSizeHeader;

  for ( unsigned int i = 0; i < mNbTotalFiles; i++ ) {
    if ( !( mFiles[i]->Truncate( truncate_offset ).IsOK() ) ) {
      eos_err( "error=error while truncating" );
      return -1;
    }
  }

  return rc;
}


/*
OBS:: can be used if updates are allowed
// -----------------------------------------------------------------------------
// Recompute and write to files the parity blocks of the groups between the two limits
// -----------------------------------------------------------------------------
int
RaidDpFile::updateParityForGroups(off_t offsetStart, off_t offsetEnd)
{
  off_t offset_group;
  off_t offset_block;

  eos::common::Timing up("parity");

  for (unsigned int i = (offsetStart / mSizeGroup); i < ceil((offsetEnd * 1.0) / mSizeGroup); i++)
  {
    offset_group = i * mSizeGroup;
    for(unsigned int j = 0; j < mNbDataBlocks; j++)
    {
      XrdOucString block = "block-"; block += (int)j;
      TIMING(block.c_str(),&up);

      offset_block = offset_group + j * mStripeWidth;
      read(offset_block, mDataBlocks[MapSmallToBig(j)], mStripeWidth);
      block += "-read";
      TIMING(block.c_str(),&up);
    }

    TIMING("Compute-In",&up);
    //do computations of parity blocks
    ComputeParity();
    TIMING("Compute-Out",&up);

    //write parity blocks to files
    WriteParityToFiles(offset_group);
    TIMING("WriteParity",&up);
  }
  //  up.Print();
  return SFS_OK;
}


// -----------------------------------------------------------------------------
// Use simple parity to recover the block - NOT USED!!
// -----------------------------------------------------------------------------
bool
RaidDpFile::SimpleParityRecover( off_t                    offsetInit,
                                 char*                    buffer,
                                 std::map<off_t, size_t>& rMapToRecover,
                                 unsigned int&            blocksCorrupted )
{
  size_t length;
  unsigned int id_corrupted = 0;
  off_t offset = rMapToRecover.begin()->first;
  off_t offset_local = ( offset / ( mNbDataFiles * mStripeWidth ) ) * mStripeWidth;
  std::map<uint64_t, uint32_t> mapErrors;

  blocksCorrupted = 0;

  // ---------------------------------------------------------------------------
  // Reset the read and write handlers
  // ---------------------------------------------------------------------------
  for ( unsigned int i = 0; i < mNbTotalFiles; i++ ) {
    mReadHandlers[i]->Reset();
    mWriteHandlers[i]->Reset();
  }

  for ( unsigned int i = 0; i < mNbTotalFiles; i++ ) {
    memset( mDataBlocks[i], 0, mStripeWidth );
    mReadHandlers[i]->Increment();
    mFiles[mapSU[i]]->Read( offset_local + mSizeHeader, mStripeWidth,
                             mDataBlocks[i], mReadHandlers[i] );
  }

  // ---------------------------------------------------------------------------
  // Mark the corrupted blocks
  // ---------------------------------------------------------------------------
  for ( unsigned int i = 0; i < mNbTotalFiles; i++ ) {
    if ( !mReadHandlers[i]->WaitOK() ) {
      id_corrupted = i;
      blocksCorrupted++;

      if ( blocksCorrupted > 1 ) {
        break;
      }
    }
  }

  if ( blocksCorrupted == 0 )
    return true;
  else if ( blocksCorrupted >= mNbParityFiles )
    return false;

  // ----------------------------------------------------------------------------
  // Use simple parity to recover
  // ----------------------------------------------------------------------------
  OperationXOR( mDataBlocks[( id_corrupted + 1 ) % ( mNbDataFiles + 1 )],
                mDataBlocks[( id_corrupted + 2 ) % ( mNbDataFiles + 1 )],
                mDataBlocks[id_corrupted],
                mStripeWidth );

  for ( unsigned int i = 3, index = ( id_corrupted + i ) % ( mNbDataFiles + 1 );
        i < ( mNbDataFiles + 1 ) ;
        i++, index = ( id_corrupted + i ) % ( mNbDataFiles + 1 ) )
  {
    OperationXOR( mDataBlocks[id_corrupted],
                  mDataBlocks[index],
                  mDataBlocks[id_corrupted],
                  mStripeWidth );
  }

  // -----------------------------------------------------------------------
  // Return recovered block and also write it to the file
  // -----------------------------------------------------------------------
  off_t offset_block = ( offset / ( mNbDataFiles * mStripeWidth ) ) * ( mNbDataFiles * mStripeWidth ) +
                id_corrupted * mStripeWidth;


  if ( mStoreRecovery ) {
    mWriteHandlers[id_corrupted]->Increment();
    mFiles[mapSU[id_corrupted]]->Write( offset_local + mSizeHeader, mStripeWidth,
                                     mDataBlocks[id_corrupted], mWriteHandlers[id_corrupted] );
  }

  for ( std::map<off_t, size_t>::iterator iter = rMapToRecover.begin();
        iter != rMapToRecover.end();
        iter++ ) {
    offset = iter->first;
    length = iter->second;

    // -----------------------------------------------------------------------
    // If not SP or DP, maybe we have to return it
    // -----------------------------------------------------------------------
    char* pBuff;
    if ( id_corrupted < mNbDataFiles ) {
      if ( ( offset >= offset_block ) &&
           ( offset < static_cast<off_t>( offset_block +  mStripeWidth ) ) ) {
        pBuff = buffer + ( offset - offsetInit );
        pBuff = static_cast<char*>( memcpy( pBuff, mDataBlocks[id_corrupted] +
        ( offset % mStripeWidth ), length ) );
      }
    }
  }

  // --------------------------------------------------------------------
  // Wait for write responses and reset all handlers
  // --------------------------------------------------------------------
  for ( unsigned int i = 0; i < mNbTotalFiles; i++ ) {
    if ( !mWriteHandlers[i]->WaitOK() ) {
      eos_err( "Write stripe %s- write failed", mStripeUrls[mapSU[i]].c_str() );
      return false;
    }

    mWriteHandlers[i]->Reset();
    mReadHandlers[i]->Reset();
  }

  return true;
}

*/


EOSFSTNAMESPACE_END

