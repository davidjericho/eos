// -----------------------------------------------------------------------------
// File: ReedSFile.cc
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
#include "fst/io/ReedSFile.hh"
#include "common/Timing.hh"
/*----------------------------------------------------------------------------*/
#include <cmath>
#include <map>
#include <set>
#include <fcntl.h>
#include "fst/zfec/fec.h"
/*----------------------------------------------------------------------------*/

EOSFSTNAMESPACE_BEGIN

// -----------------------------------------------------------------------------
// Constructor
// -----------------------------------------------------------------------------
ReedSFile::ReedSFile( std::vector<std::string> stripeUrl,
                      int                      numParity,
                      bool                     storeRecovery,
                      bool                     isStreaming,
                      off_t                    targetSize,
                      std::string              bookingOpaque )
  : RaidIO( "reedS", stripeUrl, numParity, storeRecovery,
            isStreaming, targetSize, bookingOpaque )
{
  mSizeGroup = mNbDataFiles * mStripeWidth;
  mNbDataBlocks = mNbDataFiles;
  mNbTotalBlocks = mNbDataFiles + mNbParityFiles;

  for ( unsigned int i = 0; i < mNbTotalFiles; i++ ) {
    mDataBlocks.push_back( new char[mStripeWidth] );
  }
}


// -----------------------------------------------------------------------------
// Destructor
// -----------------------------------------------------------------------------
ReedSFile::~ReedSFile()
{
  for ( unsigned int i = 0; i < mNbTotalFiles; i++ ) {
    delete[] mDataBlocks[i];
  }
}


// -----------------------------------------------------------------------------
// Compute the error correction blocks
// -----------------------------------------------------------------------------
void
ReedSFile::ComputeParity()
{
  unsigned int block_nums[mNbParityFiles];
  unsigned char* outblocks[mNbParityFiles];
  const unsigned char* blocks[mNbDataFiles];

  for ( unsigned int i = 0; i < mNbDataFiles; i++ )
    blocks[i] = ( const unsigned char* ) mDataBlocks[i];

  for ( unsigned int i = 0; i < mNbParityFiles; i++ ) {
    block_nums[i] = mNbDataFiles + i;
    outblocks[i] = ( unsigned char* ) mDataBlocks[mNbDataFiles + i];
    memset( mDataBlocks[mNbDataFiles + i], 0, mStripeWidth );
  }

  fec_t* const fec = fec_new( mNbDataFiles, mNbTotalFiles );
  fec_encode( fec, blocks, outblocks, block_nums, mNbParityFiles, mStripeWidth );

  //free memory
  fec_free( fec );
}


// -----------------------------------------------------------------------------
// Try to recover the block at the current offset
// -----------------------------------------------------------------------------
bool
ReedSFile::RecoverPieces( off_t                    offsetInit,
                          char*                    pBuffer,
                          std::map<off_t, size_t>& rMapErrors )
{
  unsigned int num_blocks_corrupted;
  vector<unsigned int> valid_ids;
  vector<unsigned int> invalid_ids;
  off_t offset = rMapErrors.begin()->first;
  size_t length = rMapErrors.begin()->second;
  off_t offset_local = ( offset / mSizeGroup ) * mStripeWidth;
  off_t offset_group = ( offset / mSizeGroup ) * mSizeGroup;
  num_blocks_corrupted = 0;

  for ( unsigned int i = 0; i < mNbTotalFiles; i++ ) {
    mReadHandlers[i]->Reset();
    mReadHandlers[i]->Increment();
    mFiles[mapSU[i]]->Read( offset_local +  mSizeHeader, mStripeWidth,
                               mDataBlocks[i], mReadHandlers[i] );
  }

  //............................................................................
  // Wait for read responses and mark corrupted blocks
  //............................................................................
  for ( unsigned int i = 0; i < mNbTotalFiles; i++ ) {
    if ( !mReadHandlers[i]->WaitOK() ) {
      eos_err( "Read stripe %s - corrupted block", mStripeUrls[mapSU[i]].c_str() );
      invalid_ids.push_back( i );
      num_blocks_corrupted++;
    } else {
      valid_ids.push_back( i );
    }

    mReadHandlers[i]->Reset();
  }

  if ( num_blocks_corrupted == 0 )
    return true;
  else if ( num_blocks_corrupted > mNbParityFiles )
    return false;
  
  //............................................................................
  // ******* DECODE ******
  //............................................................................
  const unsigned char* inpkts[mNbTotalFiles - num_blocks_corrupted];
  unsigned char* outpkts[mNbParityFiles];
  unsigned indexes[mNbDataFiles];
  bool found = false;

  //............................................................................
  // Obtain a valid combination of blocks suitable for recovery
  //............................................................................
  Backtracking( 0, indexes, valid_ids );

  for ( unsigned int i = 0; i < mNbDataFiles; i++ ) {
    inpkts[i] = ( const unsigned char* ) mDataBlocks[indexes[i]];
  }

  //............................................................................
  // Add the invalid data blocks to be recovered
  //............................................................................
  int countOut = 0;
  bool data_corrupted = false;
  bool parity_corrupted = false;

  for ( unsigned int i = 0; i < invalid_ids.size(); i++ ) {
    outpkts[i] = ( unsigned char* ) mDataBlocks[invalid_ids[i]];
    countOut++;

    if ( invalid_ids[i] >= mNbDataFiles )
      parity_corrupted = true;
    else
      data_corrupted = true;
  }

  for ( vector<unsigned int>::iterator iter = valid_ids.begin();
        iter != valid_ids.end();
        ++iter ) {
    found = false;

    for ( unsigned int i = 0; i < mNbDataFiles; i++ ) {
      if ( indexes[i] == *iter ) {
        found = true;
        break;
      }
    }

    if ( !found ) {
      outpkts[countOut] = ( unsigned char* ) mDataBlocks[*iter];
      countOut++;
    }
  }

  //............................................................................
  // Actual decoding - recover primary blocks
  //............................................................................
  if ( data_corrupted ) {
    fec_t* const fec = fec_new( mNbDataFiles, mNbTotalFiles );
    fec_decode( fec, inpkts, outpkts, indexes, mStripeWidth );
    fec_free( fec );
  }

  //............................................................................
  // If there are also parity blocks corrupted then we encode again the blocks
  // - recover secondary blocks
  //............................................................................
  if ( parity_corrupted ) {
    ComputeParity();
  }

  //............................................................................
  // Update the files in which we found invalid blocks
  //............................................................................
  char* pBuff ;
  unsigned int stripe_id;

  for ( vector<unsigned int>::iterator iter = invalid_ids.begin();
        iter != invalid_ids.end();
        ++iter ) {
    stripe_id = *iter;
    eos_debug( "Invalid index stripe: %i", stripe_id );
    eos_debug( "Writing to remote file stripe: %i, fstid: %i", stripe_id, mapSU[stripe_id] );

    if ( mStoreRecovery ) {
      mWriteHandlers[stripe_id]->Reset();
      mWriteHandlers[stripe_id]->Increment();
      mFiles[mapSU[stripe_id]]->Write( offset_local + mSizeHeader, mStripeWidth,
                                          mDataBlocks[stripe_id], mWriteHandlers[stripe_id] );
    }

    //..........................................................................
    // Write the correct block to the reading buffer, if it is not parity info
    //..........................................................................
    if ( *iter < mNbDataFiles ) { //if one of the data blocks
      for ( std::map<off_t, size_t>::iterator itPiece = rMapErrors.begin();
            itPiece != rMapErrors.end();
            itPiece++ ) {
        offset = itPiece->first;
        length = itPiece->second;

        if ( ( offset >= ( off_t )( offset_group + ( *iter ) * mStripeWidth ) ) &&
             ( offset < ( off_t )( offset_group + ( ( *iter ) + 1 ) * mStripeWidth ) ) ) {
          pBuff = pBuffer + ( offset - offsetInit );
          memcpy( pBuff, mDataBlocks[*iter] + ( offset % mStripeWidth ), length );
        }
      }
    }
  }

  //............................................................................
  // Wait for write responses
  //............................................................................
  for ( vector<unsigned int>::iterator iter = invalid_ids.begin();
        iter != invalid_ids.end();
        ++iter ) {
    if ( !mWriteHandlers[*iter]->WaitOK() ) {
      eos_err( "ReedSRecovery - write stripe failed" );
      return false;
    }
  }

  mDoneRecovery = true;
  return true;
}


// -----------------------------------------------------------------------------
// Get backtracking solution
// -----------------------------------------------------------------------------
bool
ReedSFile::SolutionBkt( unsigned int         k,
                        unsigned int*        pIndexes,
                        vector<unsigned int> validId )
{
  bool found = false;

  if ( k != mNbDataFiles ) return found;

  for ( unsigned int i = mNbDataFiles; i < mNbTotalFiles; i++ ) {
    if ( find( validId.begin(), validId.end(), i ) != validId.end() ) {
      found = false;

      for ( unsigned int j = 0; j <= k; j++ ) {
        if ( pIndexes[j] == i ) {
          found  = true;
          break;
        }
      }

      if ( !found ) break;
    }
  }

  return found;
}


// -----------------------------------------------------------------------------
// Validation function for backtracking
// -----------------------------------------------------------------------------
bool
ReedSFile::ValidBkt( unsigned int         k,
                     unsigned int*        pIndexes,
                     vector<unsigned int> validId )
{
  //............................................................................
  // Obs: condition from zfec implementation:
  // If a primary block, i, is present then it must be at index i;
  // Secondary blocks can appear anywhere.
  //............................................................................
  if ( find( validId.begin(), validId.end(), pIndexes[k] ) == validId.end() ||
       ( ( pIndexes[k] < mNbDataFiles ) && ( pIndexes[k] != k ) ) )
    return false;

  for ( unsigned int i = 0; i < k; i++ ) {
    if ( pIndexes[i] == pIndexes[k] || ( pIndexes[i] < mNbDataFiles && pIndexes[i] != i ) )
      return false;
  }

  return true;
}


// -----------------------------------------------------------------------------
// Backtracking method to get the indices needed for recovery
// -----------------------------------------------------------------------------
bool
ReedSFile::Backtracking( unsigned int         k,
                         unsigned int*        pIndexes,
                         vector<unsigned int> validId )
{
  if ( SolutionBkt( k, pIndexes, validId ) )
    return true;
  else {
    for ( pIndexes[k] = 0; pIndexes[k] < mNbTotalFiles; pIndexes[k]++ ) {
      if ( ValidBkt( k, pIndexes, validId ) )
        if ( Backtracking( k + 1, pIndexes, validId ) )
          return true;
    }

    return false;
  }
}


// -----------------------------------------------------------------------------
// Writing a file in streaming mode
// Add a new data used to compute parity block
// -----------------------------------------------------------------------------
void
ReedSFile::AddDataBlock( off_t offset, char* pBuffer, size_t length )
{
  int indx_block;
  size_t nwrite;
  off_t offset_in_block;
  off_t offset_in_group = offset % mSizeGroup;

  //............................................................................
  // In case the file is smaller than mSizeGroup, we need to force it to compute
  // the parity blocks
  //............................................................................
  if ( ( mOffGroupParity == -1 ) && ( offset < static_cast<off_t>( mSizeGroup ) ) ) {
    mOffGroupParity = 0;
  }

  if ( offset_in_group == 0 ) {
    mFullDataBlocks = false;

    for ( unsigned int i = 0; i < mNbDataFiles; i++ ) {
      memset( mDataBlocks[i], 0, mStripeWidth );
    }
  }

  char* ptr;
  size_t availableLength;

  while ( length ) {
    offset_in_block = offset_in_group % mStripeWidth;
    availableLength = mStripeWidth - offset_in_block;
    indx_block = offset_in_group / mStripeWidth;
    nwrite = ( length > availableLength ) ? availableLength : length;
    ptr = mDataBlocks[indx_block];
    ptr += offset_in_block;
    ptr = ( char* )memcpy( ptr, pBuffer, nwrite );
    offset += nwrite;
    length -= nwrite;
    pBuffer += nwrite;
    offset_in_group = offset % mSizeGroup;

    if ( offset_in_group == 0 ) {
      //........................................................................
      // We completed a group, we can compute parity
      //........................................................................
      mOffGroupParity = ( ( offset - 1 ) / mSizeGroup ) *  mSizeGroup;
      mFullDataBlocks = true;
      DoBlockParity( mOffGroupParity );
      mOffGroupParity = ( offset / mSizeGroup ) *  mSizeGroup;

      for ( unsigned int i = 0; i < mNbDataFiles; i++ ) {
        memset( mDataBlocks[i], 0, mStripeWidth );
      }
    }
  }
}


// -----------------------------------------------------------------------------
// Write the parity blocks from mDataBlocks to the corresponding file stripes
// -----------------------------------------------------------------------------
int
ReedSFile::WriteParityToFiles( off_t offsetGroup )
{
  off_t offset_local = offsetGroup / mNbDataFiles;

  for ( unsigned int i = mNbDataFiles; i < mNbTotalFiles; i++ ) {
    mWriteHandlers[i]->Reset();
    mWriteHandlers[i]->Increment();
    mFiles[mapSU[i]]->Write( offset_local + mSizeHeader, mStripeWidth,
                                mDataBlocks[i], mWriteHandlers[i] );
  }

  for ( unsigned int i = mNbDataFiles; i < mNbTotalFiles; i++ ) {
    if ( !mWriteHandlers[i]->WaitOK() ) {
      eos_err( "ReedSWrite write local stripe - write failed" );
      return -1;
    }
  }

  return SFS_OK;
}


// -----------------------------------------------------------------------------
// Truncate file
// -----------------------------------------------------------------------------
int
ReedSFile::truncate( off_t offset )
{
  int rc = SFS_OK;
  off_t truncateOffset = 0;

  if ( !offset ) return rc;

  truncateOffset = ceil( ( offset * 1.0 ) / mSizeGroup ) * mStripeWidth;
  truncateOffset += mSizeHeader;

  for ( unsigned int i = 0; i < mNbTotalFiles; i++ ) {
    if ( !( mFiles[i]->Truncate( truncateOffset ).IsOK() ) ) {
      eos_err( "error=error while truncating" );
      return -1;
    }
  }

  return rc;
}


// -----------------------------------------------------------------------------
// Return the same index in the Reed-Solomon case
// -----------------------------------------------------------------------------
unsigned int
ReedSFile::MapSmallToBig( unsigned int idSmall )
{
  if ( idSmall >= mNbDataBlocks ) {
    eos_err( "error=idSmall bigger than expected" );
    return -1;
  }

  return idSmall;
}


/*
OBS:: can be used if updated are allowed
// -----------------------------------------------------------------------------
// Recompute and write to files the parity blocks of the groups between the two limits
// -----------------------------------------------------------------------------
int
ReedSFile::updateParityForGroups(off_t offsetStart, off_t offsetEnd)
{
  off_t offset_group;
  off_t offset_block;

  for (unsigned int i = (offsetStart / mSizeGroup);
       i < ceil((offsetEnd * 1.0 ) / mSizeGroup); i++)
  {
    offset_group = i * mSizeGroup;
    for(unsigned int j = 0; j < mNbDataFiles; j++)
    {
      offset_block = offset_group + j * mStripeWidth;
      read(offset_block, mDataBlocks[j], mStripeWidth);
    }

    //compute parity blocks and write to files
    ComputeParity();
    writeParityToFiles(offset_group/mNbDataFiles);
  }

  return SFS_OK;
}
*/

EOSFSTNAMESPACE_END

