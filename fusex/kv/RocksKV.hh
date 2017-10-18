//------------------------------------------------------------------------------
//! @file RocksKV.hh
//! @author Georgios Bitzes CERN
//! @brief kv persistency class based on rocksdb
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

#ifndef FUSE_ROCKS_KV_HH_
#define FUSE_ROCKS_KV_HH_

#include <sys/stat.h>
#include <sys/types.h>
#include "llfusexx.hh"
#include "fusex/fusex.pb.h"
#include "kv/kv.hh"
#include "XrdSys/XrdSysPthread.hh"
#include <memory>
#include <map>
#include <event.h>
#include <rocksdb/db.h>


//------------------------------------------------------------------------------
// Implementation of the key value store interface based on redis
//------------------------------------------------------------------------------
class RocksKV : public kv
{
public:
  RocksKV();
  virtual ~RocksKV();

  int connect(const std::string &path);

  int get(std::string &key, std::string &value) override;
  int get(std::string &key, uint64_t &value) override;
  int put(std::string &key, std::string &value) override;
  int put(std::string &key, uint64_t &value) override;
  int inc(std::string &key, uint64_t &value) override;

  int erase(std::string &key) override;

  int get(uint64_t key, std::string &value, std::string name_space="i") override;
  int put(uint64_t key, std::string &value, std::string name_space="i") override;

  int get(uint64_t key, uint64_t &value, std::string name_space="i") override;
  int put(uint64_t key, uint64_t &value, std::string name_space="i") override;

  int erase(uint64_t key, std::string name_space="i") override;

  std::string prefix(std::string& key) { return mPrefix+key; }
private:
  std::unique_ptr<rocksdb::DB> db;
  std::string mPrefix;
} ;
#endif /* FUSE_KV_HH_ */
