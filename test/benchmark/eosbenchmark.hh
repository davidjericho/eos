//------------------------------------------------------------------------------
// File: eosbenchmark.hh
// Author: Elvin-Alin Sindrilaru - CERN
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

#ifndef __BMK_EOSBENCHMARK_HH__
#define __BMK_EOSBENCHMARK_HH__

/*-----------------------------------------------------------------------------*/
#include <cstdint>
/*-----------------------------------------------------------------------------*/
#include "Namespace.hh"
/*-----------------------------------------------------------------------------*/

EOSBMKNAMESPACE_BEGIN

//! Forward declaration
class Configuration;
class Result;

//! Function signature to be excuted by the theads
typedef void* ( *TypeFunc )( void* );


//------------------------------------------------------------------------------
//! Structure containg the configuration to be excuted and the result
//! data structure
//------------------------------------------------------------------------------
struct ConfResStruct {
  Configuration& config;
  Result&        result;
};


//------------------------------------------------------------------------------
//! Structure containg the configuration to be excuted and the id of the thread
//! responsible for the execution
//------------------------------------------------------------------------------
struct ConfIdStruct {
  Configuration& config;
  uint32_t       id;

  //----------------------------------------------------------------------------
  //! Constructor
  //----------------------------------------------------------------------------
  ConfIdStruct(Configuration& conf, uint32_t index):
    config(conf),
    id(index)
  {
    // empty
  }
};


//------------------------------------------------------------------------------
//! Print the usage instructions for eosbenchmark command 
//------------------------------------------------------------------------------
void Usage();


//------------------------------------------------------------------------------
//! Print results from file filtering by the configuration
//!
//! @param resultsFile file containing config<-->results paris from past runs
//! @param configFile file containing a specific configuration to be displayed
//!
//------------------------------------------------------------------------------
void PrintResults(const std::string& resultsFile, const std::string& configFile);


EOSBMKNAMESPACE_END

#endif // __BMK_EOSBENCHMARK_HH__
