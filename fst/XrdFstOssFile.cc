//------------------------------------------------------------------------------
// File XrdFstOssFile.cc
// Author Elvin-Alin Sindrilaru - CERN
//------------------------------------------------------------------------------

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
#include <fcntl.h>
/*----------------------------------------------------------------------------*/
#include "fst/XrdFstOss.hh"
#include "fst/XrdFstOssFile.hh"
#include "fst/checksum/ChecksumPlugins.hh"
/*----------------------------------------------------------------------------*/

EOSFSTNAMESPACE_BEGIN

//! pointer to the current OSS implementation to be used by the oss files
extern XrdFstOss* XrdFstSS;

//------------------------------------------------------------------------------
// Constuctor
//------------------------------------------------------------------------------
XrdFstOssFile::XrdFstOssFile( const char* tid ):
  XrdOssFile( tid ),
  eos::common::LogId(),
  mIsRW( false ),
  mRWLockXs( 0 ),
  mBlockXs( 0 )
{
  // empty
}


//------------------------------------------------------------------------------
// Destructor
//------------------------------------------------------------------------------
XrdFstOssFile::~XrdFstOssFile()
{
  // empty
}


//------------------------------------------------------------------------------
// Open function
//------------------------------------------------------------------------------
int
XrdFstOssFile::Open( const char* path, int flags, mode_t mode, XrdOucEnv& env )
{
  const char* val = 0;
  unsigned long lid = 0;
  off_t booking_size = 0;
  mPath = path;

  if ( ( val = env.Get( "mgm.lid" ) ) ) {
    lid = atol( val );
  }

  if ( ( val = env.Get( "mgm.bookingsize" ) ) ) {
    booking_size = strtoull( val, 0, 10 );

    if ( errno == ERANGE ) {
      eos_err( "error=invalid bookingsize in capability: %s", val );
      return -EINVAL;
    }
  }

  //............................................................................
  // Decide if file opened for rw operations
  //............................................................................
  if ( ( flags &
         ( O_RDONLY | O_WRONLY | O_RDWR | O_CREAT  | O_TRUNC ) ) != 0 ) {
    mIsRW = true;
  }
  
  if ( eos::common::LayoutId::GetBlockChecksum( lid ) != eos::common::LayoutId::kNone ) {
    //..........................................................................
    // Look for a blockchecksum obj corresponding to this file
    //..........................................................................
    std::pair<XrdSysRWLock*, CheckSum*> pair_value;
    pair_value = XrdFstSS->GetXsObj( path, mIsRW );
    mRWLockXs = pair_value.first;
    mBlockXs = pair_value.second;

    if ( !mBlockXs ) {
      mBlockXs = ChecksumPlugins::GetChecksumObject( lid, true );

      if ( mBlockXs ) {
        XrdOucString xs_path = mBlockXs->MakeBlockXSPath( mPath.c_str() );
        struct stat buf;
        int retc = XrdFstSS->Stat( mPath.c_str(), &buf );

        if ( !mBlockXs->OpenMap( xs_path.c_str(),
                                 ( retc ? booking_size : buf.st_size ),
                                 eos::common::LayoutId::OssXsBlockSize,
                                 false ) ) {
          eos_err( "error=unable to open the blockchecksum file: %s",
                   xs_path.c_str() );
          return -EIO;
        }
        
        //......................................................................
        // Add the new file blockchecksum mapping
        //......................................................................
        mRWLockXs = XrdFstSS->AddMapping( path, mBlockXs, mIsRW );
      } else {
        eos_err( "error=unable to create the blockchecksum obj" );
        return -EIO;
      }
    }
  }

  int retc = XrdOssFile::Open( path, flags, mode, env );
  return retc;
}


//------------------------------------------------------------------------------
// Read
//------------------------------------------------------------------------------
ssize_t
XrdFstOssFile::Read( void* buffer, off_t offset, size_t length )
{
  int retc = XrdOssFile::Read( buffer, offset, length );

  if ( mBlockXs ) {
    XrdSysRWLockHelper wr_lock( mRWLockXs, 0 );
    if ( ( retc > 0 ) &&
         ( !mBlockXs->CheckBlockSum( offset, static_cast<const char*>( buffer ), retc ) ) )
    {
      eos_err( "error=read block-xs error offset=%zu, length=%zu",
               offset, length );
      return -EIO;
    }
  }

  return retc;
}


//------------------------------------------------------------------------------
// Read raw
//------------------------------------------------------------------------------
ssize_t
XrdFstOssFile::ReadRaw( void* buffer, off_t offset, size_t length )
{
  ssize_t retc = XrdOssFile::ReadRaw( buffer, offset, length );

  if ( mBlockXs ) {
    XrdSysRWLockHelper wr_lock( mRWLockXs, 0 );

    if ( ( retc > 0 ) &&
         ( !mBlockXs->CheckBlockSum( offset, static_cast<const char*>( buffer ), retc ) ) )
    {
      eos_err( "error=read block-xs error offset=%zu, length=%zu",
               offset, length );
      return -EIO;
    }
  }

  return retc;
}


//------------------------------------------------------------------------------
// Write
//------------------------------------------------------------------------------
ssize_t
XrdFstOssFile::Write( const void* buffer, off_t offset, size_t length )
{
  if ( mBlockXs ) {
    XrdSysRWLockHelper wr_lock( mRWLockXs, 0 );
    mBlockXs->AddBlockSum( offset, static_cast<const char*>( buffer ), length );
  }

  ssize_t retc = XrdOssFile::Write( buffer, offset, length );
  return retc;
}


//------------------------------------------------------------------------------
// Close function
//------------------------------------------------------------------------------
int
XrdFstOssFile::Close( long long* retsz )
{
  int retc = 0;
  bool delete_mapping = false;

  //............................................................................
  // Code dealing with block checksums
  //............................................................................
  if ( mBlockXs ) {
    struct stat statinfo;

    if ( ( XrdFstSS->Stat( mPath.c_str(), &statinfo ) ) ) {
      eos_err( "error=close - cannot stat closed file: %s", mPath.c_str() );
      return XrdOssFile::Close( retsz );
    }

    XrdSysRWLockHelper wr_lock( mRWLockXs );                // ---> wrlock xs obj
    mBlockXs->DecrementRef( mIsRW );

    if ( mBlockXs->GetTotalRef() >= 1 ) {
      //........................................................................
      // If multiple references
      //........................................................................
      if ( ( mBlockXs->GetNumRef( true ) == 0 ) && mIsRW ) {
        //......................................................................
        // If one last writer and this is the current one
        //......................................................................
        if ( !mBlockXs->ChangeMap( statinfo.st_size, true ) ) {
          eos_err( "error=unable to change block checksum map" );
          retc = -1;
        } else {
          eos_info( "info=\"adjusting block XS map\"" );
        }

        if ( !mBlockXs->AddBlockSumHoles( getFD() ) ) {
          eos_warning( "warning=unable to fill holes of block checksum map" );
        }
      }
    } else {
      //........................................................................
      // Just one reference left (the current one)
      //........................................................................
      if ( mIsRW ) {
        if ( !mBlockXs->ChangeMap( statinfo.st_size, true ) ) {
          eos_err( "error=Unable to change block checksum map" );
          retc = 1;
        } else {
          eos_info( "info=\"adjusting block XS map\"" );
        }

        if ( !mBlockXs->AddBlockSumHoles( getFD() ) ) {
          eos_warning( "warning=unable to fill holes of block checksum map" );
        }
      }

      if ( !mBlockXs->CloseMap() ) {
        eos_err( "error=unable to close block checksum map" );
        retc = 1;
      }

      delete_mapping = true;
    }
  }

  //............................................................................
  // Delete the filename - xs obj mapping from Oss if required
  //............................................................................
  if ( delete_mapping ) {
    eos_debug( "Delete entry from oss map" );
    XrdFstSS->DropXs( mPath.c_str() );
  } else {
    eos_debug( "No delete from oss map" );
  }

  retc |= XrdOssFile::Close( retsz );
  return retc;
}

EOSFSTNAMESPACE_END
