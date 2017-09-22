//------------------------------------------------------------------------------
//! @file dircleaner.hh
//! @author Andreas-Joachim Peters CERN
//! @brief class keeping dir contents at a given level
//------------------------------------------------------------------------------

/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2016 CERN/Switzerland                                  *
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

#ifndef FUSE_DIRCLEANER_HH_
#define FUSE_DIRCLEANER_HH_

#include <sys/stat.h>
#include <sys/types.h>
#include "common/Logging.hh"
#include <memory>
#include <map>
#include <set>
#include <atomic>
#include <exception>
#include <stdexcept>
#include <thread>

class dircleaner
{
public:

  typedef struct fileinfo
  {
    std::string path;
    time_t mtime;
    size_t size;
  } file_info_t;
  
  typedef std::multimap<time_t, file_info_t> tree_map_t; 
  
  typedef struct tree_info
  {
    tree_info() {totalsize=0; totalfiles=0;}
    tree_map_t treemap;
    uint64_t totalsize;
    uint64_t totalfiles;
    std::string path;
    
    void Print(std::string& out);
    
  } tree_info_t;
  
  dircleaner();
  virtual ~dircleaner();
  
  int cleanall(const std::string path);
  int scanall(const std::string path, dircleaner::tree_info_t& treeinfo); 
  
  //----------------------------------------------------------------------------
};
#endif