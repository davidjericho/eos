//------------------------------------------------------------------------------
// File: EosAuthOfsFile.cc
// Author: Elvin-Alin Sindrilau <esindril@cern.ch> CERN
//------------------------------------------------------------------------------

/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2013 CERN/Switzerland                                  *
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
#include "EosAuthOfsFile.hh"
#include "EosAuthOfs.hh"
#include "ProtoUtils.hh"
/*----------------------------------------------------------------------------*/
#include <sstream>
/*----------------------------------------------------------------------------*/

EOSAUTHNAMESPACE_BEGIN

//------------------------------------------------------------------------------
// Constructor
//------------------------------------------------------------------------------
EosAuthOfsFile::EosAuthOfsFile(char *user, int MonID):
    XrdSfsFile(user, MonID),
    eos::common::LogId(),
    mName("")
{
  // emtpy
}

  
//------------------------------------------------------------------------------
// Destructor
//------------------------------------------------------------------------------
EosAuthOfsFile::~EosAuthOfsFile()
{
  // empty
}


//------------------------------------------------------------------------------
// Open a file
//------------------------------------------------------------------------------
int
EosAuthOfsFile::open(const char* fileName,
                     XrdSfsFileOpenMode openMode,
                     mode_t createMode,
                     const XrdSecEntity* client,
                     const char* opaque)
{
  int retc;
  eos_debug("file open name=%s opaque=%s", fileName, opaque);
  mName = fileName;

  // Get a socket object from the pool
  zmq::socket_t* socket;
  gOFS->mPoolSocket.wait_pop(socket);
  std::ostringstream sstr;
  sstr << this;
  eos_debug("file pointer: %s", sstr.str().c_str());
  RequestProto* req_proto = utils::GetFileOpenRequest(sstr.str(), fileName, openMode,
                                                      createMode, client, opaque,
                                                      error.getErrUser(), error.getErrMid());
     
  if (!gOFS->SendProtoBufRequest(socket, req_proto))
  {
    eos_err("file open - unable to send request");
    return SFS_ERROR;
  }

  ResponseProto* resp_open = static_cast<ResponseProto*>(gOFS->GetResponse(socket));
  retc = resp_open->response();
  eos_debug("got response for file open request: %i", retc);

  delete resp_open;
  delete req_proto;

  // Put back the socket object in the pool
  gOFS->mPoolSocket.push(socket);
  return retc;
}

  
//------------------------------------------------------------------------------
// Read function
//------------------------------------------------------------------------------
XrdSfsXferSize
EosAuthOfsFile::read(XrdSfsFileOffset offset,
                     char* buffer,
                     XrdSfsXferSize length)
{
  int retc;
  eos_debug("read off=%li len=%i", (long long)offset, (int)length);

  // Get a socket object from the pool
  zmq::socket_t* socket;
  gOFS->mPoolSocket.wait_pop(socket);
  std::ostringstream sstr;
  sstr << this;
  eos_debug("file pointer=%s, offset=%li, length=%i", sstr.str().c_str(), offset, length);
  RequestProto* req_proto = utils::GetFileReadRequest(sstr.str(), offset, length);
     
  if (!gOFS->SendProtoBufRequest(socket, req_proto))
  {
    eos_err("file read - unable to send request");
    return 0;
  }

  ResponseProto* resp_fread = static_cast<ResponseProto*>(gOFS->GetResponse(socket));
  retc = resp_fread->response();
  eos_debug("got response for file read request: %i and data is: %s",
            retc, resp_fread->message().c_str());

  if (resp_fread->has_message())
  {
    buffer = static_cast<char*>(memcpy((void*)buffer,
                                       resp_fread->message().c_str(),
                                       resp_fread->message().length()));
  }
  
  delete resp_fread;
  delete req_proto;

  // Put back the socket object in the pool
  gOFS->mPoolSocket.push(socket);
  return retc;  
}


//------------------------------------------------------------------------------
// Write function
//------------------------------------------------------------------------------
XrdSfsXferSize
EosAuthOfsFile::write(XrdSfsFileOffset offset,
                      const char* buffer,
                      XrdSfsXferSize length)
{
  int retc;
  eos_debug("write off=%ll len=%i", offset, length);

  // Get a socket object from the pool
  zmq::socket_t* socket;
  gOFS->mPoolSocket.wait_pop(socket);
  std::ostringstream sstr;
  sstr << this;
  eos_debug("file pointer: %s", sstr.str().c_str());
  RequestProto* req_proto = utils::GetFileWriteRequest(sstr.str(), offset, buffer, length);
     
  if (!gOFS->SendProtoBufRequest(socket, req_proto))
  {
    eos_err("file write - unable to send request");
    return 0;
  }

  ResponseProto* resp_fwrite = static_cast<ResponseProto*>(gOFS->GetResponse(socket));
  retc = resp_fwrite->response();
  eos_debug("got response for file write request");
  delete resp_fwrite;
  delete req_proto;

  // Put back the socket object in the pool
  gOFS->mPoolSocket.push(socket);
  return retc;
}


//------------------------------------------------------------------------------
// Get name of file
//------------------------------------------------------------------------------
const char*
EosAuthOfsFile::FName()
{
  int retc;
  eos_debug("file fname");

  // Get a socket object from the pool
  zmq::socket_t* socket;
  gOFS->mPoolSocket.wait_pop(socket);
  std::ostringstream sstr;
  sstr << this;
  eos_debug("file pointer: %s", sstr.str().c_str());
  RequestProto* req_proto = utils::GetFileFnameRequest(sstr.str());
     
  if (!gOFS->SendProtoBufRequest(socket, req_proto))
  {
    eos_err("file fname - unable to send request");
    return static_cast<const char*>(0);
  }

  ResponseProto* resp_fname = static_cast<ResponseProto*>(gOFS->GetResponse(socket));
  retc = resp_fname->response();
  eos_debug("got response for filefname request");

  if (retc == SFS_ERROR)
  {
    eos_debug("file fname not found or error on server side");
    return static_cast<const char*>(0);
  }
  else 
  {
    eos_debug("file fname is: %s", resp_fname->message().c_str());
    mName = resp_fname->message();
  }

  delete resp_fname;
  delete req_proto;

  // Put back the socket object in the pool
  gOFS->mPoolSocket.push(socket);
  return mName.c_str();  
}


//------------------------------------------------------------------------------
// Stat function
//------------------------------------------------------------------------------
int
EosAuthOfsFile::stat(struct stat *buf)
{
  int retc;
  eos_debug("stat file name=%s", mName.c_str());

  // Get a socket object from the pool
  zmq::socket_t* socket;
  gOFS->mPoolSocket.wait_pop(socket);
  std::ostringstream sstr;
  sstr << this;
  eos_debug("file pointer: %s", sstr.str().c_str());
  RequestProto* req_proto = utils::GetFileStatRequest(sstr.str());
     
  if (!gOFS->SendProtoBufRequest(socket, req_proto))
  {
    eos_err("file stat - unable to send request");
    memset(buf, 0, sizeof(struct stat));
    return SFS_ERROR;;
  }

  ResponseProto* resp_fstat = static_cast<ResponseProto*>(gOFS->GetResponse(socket));
  retc = resp_fstat->response();
  buf = static_cast<struct stat*>(memcpy((void*)buf,
                                         resp_fstat->message().c_str(),
                                         sizeof(struct stat)));
  eos_debug("got response for fstat request: %i", retc);
  delete resp_fstat;
  delete req_proto;

  // Put back the socket object in the pool
  gOFS->mPoolSocket.push(socket);
  return retc;  
}
  
 
//------------------------------------------------------------------------------
//! Close file 
//------------------------------------------------------------------------------
int
EosAuthOfsFile::close()
{
  int retc;
  eos_debug("close");

  // Get a socket object from the pool
  zmq::socket_t* socket;
  gOFS->mPoolSocket.wait_pop(socket);
  std::ostringstream sstr;
  sstr << this;
  eos_debug("file pointer: %s", sstr.str().c_str());
  RequestProto* req_proto = utils::GetFileCloseRequest(sstr.str());
     
  if (!gOFS->SendProtoBufRequest(socket, req_proto))
  {
    eos_err("file close - unable to send request");
    return SFS_ERROR;
  }

  ResponseProto* resp_close = static_cast<ResponseProto*>(gOFS->GetResponse(socket));
  retc = resp_close->response();
  eos_debug("got response for file close request: %i", retc);

  delete resp_close;
  delete req_proto;

  // Put back the socket object in the pool
  gOFS->mPoolSocket.push(socket);
  return retc;
}

EOSAUTHNAMESPACE_END
