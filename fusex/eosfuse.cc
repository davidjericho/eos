//------------------------------------------------------------------------------
//! @file eosfuse.cc
//! @author Andreas-Joachim Peters CERN
//! @brief EOS C++ Fuse low-level implementation (3rd generation)
//------------------------------------------------------------------------------

/************************************************************************
 * EOS - the CERN Disk Storage System                                   *
 * Copyright (C) 2016CERN/Switzerland                                  *
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

#include "eosfuse.hh"
#include "MacOSXHelper.hh"

#include <string>
#include <map>
#include <iostream>
#include <sstream>
#include <memory>
#include <algorithm>
#include <thread>
#include <iterator>

#include <dirent.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>


#include <sys/types.h>
#ifdef __APPLE__
#include <sys/xattr.h>
#else
#include <attr/xattr.h>
#endif

#include <json/json.h>

#include "common/Timing.hh"
#include "common/Logging.hh"
#include "common/Path.hh"
#include "common/LinuxMemConsumption.hh"
#include "common/LinuxStat.hh"
#include "common/StringConversion.hh"
#include "md.hh"
#include "kv.hh"
#include "cache.hh"

#include "EosFuseSessionLoop.hh"

#define _FILE_OFFSET_BITS 64 

EosFuse* EosFuse::sEosFuse = 0;

/* -------------------------------------------------------------------------- */
EosFuse::EosFuse()
{
  sEosFuse = this;
}

/* -------------------------------------------------------------------------- */
EosFuse::~EosFuse()
{
  eos_static_warning("");
}

/* -------------------------------------------------------------------------- */
int
/* -------------------------------------------------------------------------- */
EosFuse::run(int argc, char* argv[], void *userdata)
/* -------------------------------------------------------------------------- */
{
  eos_static_debug("");

  struct fuse_chan* ch;
  struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
  char* local_mount_dir = 0;
  int err = 0;

  // check the fsname to choose the right JSON config file

  std::string fsname = "";
  for (int i = 0; i < argc; i++)
  {
    std::string option = argv[i];
    size_t npos;
    size_t epos;

    if ((npos = option.find("fsname=")) != std::string::npos)
    {
      epos = option.find(",", npos);
      fsname = option.substr(npos + std::string("fsname=").length(),
                             (epos != std::string::npos) ?
                             epos - npos : option.length() - npos);
      break;
    }
  }

  fprintf(stderr, "# fsname='%s'\n", fsname.c_str());

  std::string jsonconfig = "/etc/eos/fuse";

  if (fsname.length())
  {
    jsonconfig += ".";
    jsonconfig += fsname;
  }
  jsonconfig += ".conf";

#ifndef __APPLE__
  if (::access("/bin/fusermount", X_OK))
  {
    fprintf(stderr, "error: /bin/fusermount is not executable for you!\n");
    exit(-1);
  }
#endif

  if (getuid() <= DAEMONUID)
  {
    unsetenv("KRB5CCNAME");
    unsetenv("X509_USER_PROXY");
  }

  {
    // parse JSON configuration
    Json::Value root;
    Json::Reader reader;
    std::ifstream configfile(jsonconfig, std::ifstream::binary);
    if (reader.parse(configfile, root, false))
    {
      fprintf(stderr, "# JSON parsing successfull\n");
    }
    else
    {
      fprintf(stderr, "error: invalid configuration file %s - %s\n",
              jsonconfig.c_str(), reader.getFormatedErrorMessages().c_str());
      exit(EINVAL);
    }


    const Json::Value jname = root["name"];
    config.name = root["name"].asString();
    config.hostport = root["hostport"].asString();
    config.remotemountdir = root["remotemountdir"].asString();
    config.localmountdir = root["localmountdir"].asString();
    config.statfilesuffix = root["statfilesuffix"].asString();
    config.statfilepath = root["statfilepath"].asString();
    config.options.debug = root["options"]["debug"].asInt();
    config.options.lowleveldebug = root["options"]["lowleveldebug"].asInt();
    config.options.debuglevel = root["options"]["debuglevel"].asInt();
    config.options.libfusethreads = root["options"]["libfusethreads"].asInt();
    config.options.foreground = root["options"]["foreground"].asInt();

    config.mdcachehost = root["mdcachehost"].asString();
    config.mdcacheport = root["mdcacheport"].asInt();
    // default settings
    if (!config.statfilesuffix.length())
    {
      config.statfilesuffix="stats";
    }
    if (!config.mdcachehost.length())
    {
      config.mdcachehost="localhost";
    }
    if (!config.mdcacheport)
    {
      config.mdcacheport = 6379;
    }

    // data caching configuration
    cachehandler::cacheconfig cconfig;
    cconfig.type = cachehandler::cache_t::INVALID;

    if (root["cache"]["type"].asString() == "disk")
    {
      cconfig.type = cachehandler::cache_t::DISK;
    }
    else if (root["cache"]["type"].asString() == "memory")
    {
      cconfig.type = cachehandler::cache_t::MEMORY;
    }
    else
    {
      fprintf(stderr, "error: invalid cache type configuration\n");
    }

    cconfig.location = root["cache"]["location"].asString();
    cconfig.mbsize = root["cache"]["size-mb"].asInt();

    int rc=0;

    if ( (rc = cachehandler::instance().init(cconfig)))
    {
      exit(rc);
    }
  }

  int debug;
  if ((fuse_parse_cmdline(&args, &local_mount_dir, NULL, &debug) != -1) &&
      ((ch = fuse_mount(local_mount_dir, &args)) != NULL) &&
      (fuse_daemonize(config.options.foreground) != -1))
  {
    FILE* fstderr;
    // Open log file                                                                                                                                                                                               
    if (getuid())
    {
      char logfile[1024];
      if (getenv("EOS_FUSE_LOGFILE"))
      {
        snprintf(logfile, sizeof ( logfile) - 1, "%s",
                 getenv("EOS_FUSE_LOGFILE"));
      }
      else
      {
        snprintf(logfile, sizeof ( logfile) - 1, "/tmp/eos-fuse.%d.log",
                 getuid());
      }

      if (!config.statfilepath.length())
      {
        config.statfilepath = logfile;
        config.statfilepath += ".";
        config.statfilepath += config.statfilesuffix;
      }

      // Running as a user ... we log into /tmp/eos-fuse.$UID.log                                                                                                                                                     
      if (!(fstderr = freopen(logfile, "a+", stderr)))
        fprintf(stderr, "error: cannot open log file %s\n", logfile);
      else
        ::chmod(logfile, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    }
    else
    {
      // Running as root ... we log into /var/log/eos/fuse                                                                                                                                                            
      std::string log_path = "/var/log/eos/fusex/fuse.";
      if (getenv("EOS_FUSE_LOG_PREFIX"))
      {
        log_path += getenv("EOS_FUSE_LOG_PREFIX");
        if (!config.statfilepath.length()) config.statfilepath = log_path +
                "." + config.statfilesuffix;
        log_path += ".log";
      }
      else
      {
        if (!config.statfilepath.length()) config.statfilepath = log_path +
                config.statfilesuffix;
        log_path += "log";
      }

      eos::common::Path cPath(log_path.c_str());
      cPath.MakeParentPath(S_IRWXU | S_IRGRP | S_IROTH);

      if (!(fstderr = freopen(cPath.GetPath(), "a+", stderr)))
        fprintf(stderr, "error: cannot open log file %s\n", cPath.GetPath());
      else
        ::chmod(cPath.GetPath(), S_IRUSR | S_IWUSR);
    }


    setvbuf(fstderr, (char*) NULL, _IONBF, 0);

    eos::common::Logging::Init ();
    eos::common::Logging::SetUnit ("FUSE@eosxd");
    eos::common::Logging::gShortFormat = true;

    if (config.options.debug)
    {
      eos::common::Logging::SetLogPriority(LOG_DEBUG);
    }
    else
    {
      if (config.options.debuglevel)
        eos::common::Logging::SetLogPriority(config.options.debuglevel);
      else
        eos::common::Logging::SetLogPriority(LOG_INFO);
    }

    if (config.mdcachehost.length())
    {
      if (mKV.connect(config.mdcachehost, config.mdcacheport ?
                      config.mdcacheport : 6379))
      {
        fprintf(stderr, "error: failed to connect to md cache - connect-string=%s",
                config.mdcachehost.c_str());
        exit(EINVAL);
      }
    }

    mds.init();

    fusestat.Add("getattr", 0, 0, 0);
    fusestat.Add("setattr", 0, 0, 0);
    fusestat.Add("setattr:chown", 0, 0, 0);
    fusestat.Add("setattr:chmod", 0, 0, 0);
    fusestat.Add("setattr:utimes", 0, 0, 0);
    fusestat.Add("setattr:truncate", 0, 0, 0);
    fusestat.Add("lookup", 0, 0, 0);
    fusestat.Add("opendir", 0, 0, 0);
    fusestat.Add("readdir", 0, 0, 0);
    fusestat.Add("releasedir", 0, 0, 0);
    fusestat.Add("statfs", 0, 0, 0);
    fusestat.Add("mknod", 0, 0, 0);
    fusestat.Add("mkdir", 0, 0, 0);
    fusestat.Add("rm", 0, 0, 0);
    fusestat.Add("unlink", 0, 0, 0);
    fusestat.Add("rmdir", 0, 0, 0);
    fusestat.Add("rename", 0, 0, 0);
    fusestat.Add("access", 0, 0, 0);
    fusestat.Add("open", 0, 0, 0);
    fusestat.Add("create", 0, 0, 0);
    fusestat.Add("read", 0, 0, 0);
    fusestat.Add("write", 0, 0, 0);
    fusestat.Add("release", 0, 0, 0);
    fusestat.Add("fsync", 0, 0, 0);
    fusestat.Add("forget", 0, 0, 0);
    fusestat.Add("flush", 0, 0, 0);
    fusestat.Add("getxattr", 0, 0, 0);
    fusestat.Add("setxattr", 0, 0, 0);
    fusestat.Add("listxattr", 0, 0, 0);
    fusestat.Add("removexattr", 0, 0, 0);
    fusestat.Add("readlink", 0, 0, 0);
    fusestat.Add("symlink", 0, 0, 0);
    fusestat.Add(__SUM__TOTAL__, 0, 0, 0);

    tDumpStatistic = std::thread(EosFuse::DumpStatistic);
    tDumpStatistic.detach();
    tStatCirculate = std::thread(EosFuse::StatCirculate);
    tStatCirculate.detach();
    tMetaCacheFlush = std::thread(&metad::mdcflush, &mds);
    tMetaCacheFlush.detach();

    eos_static_warning("********************************************************************************");
    eos_static_warning("eosdx started version %s - FUSE protocol version %d", VERSION, FUSE_USE_VERSION);
    eos_static_warning("eos-instance-url       := %s", config.hostport.c_str());
    eos_static_warning("thread-pool            := %s", config.options.libfusethreads ? "libfuse" : "custom");

    cachehandler::instance().logconfig();

    struct fuse_session* se;
    se = fuse_lowlevel_new(&args,
                           &(get_operations()),
                           sizeof (operations), NULL);

    if ((se != NULL))
    {
      if (fuse_set_signal_handlers(se) != -1)
      {
        fuse_session_add_chan(se, ch);

        if (getenv("EOS_FUSE_NO_MT") &&
            (!strcmp(getenv("EOS_FUSE_NO_MT"), "1")))
        {
          err = fuse_session_loop(se);
        }
        else
        {
          if (config.options.libfusethreads)
          {
            err = fuse_session_loop_mt(se);
          }
          else
          {
            EosFuseSessionLoop loop( 10, 20, 10, 20 );
            err = loop.Loop( se );
          }
        }

        fuse_remove_signal_handlers(se);
        fuse_session_remove_chan(ch);
      }
      fuse_session_destroy(se);
    }

    eos_static_warning("eosdx stopped version %s - FUSE protocol version %d", VERSION, FUSE_USE_VERSION);
    eos_static_warning("********************************************************************************");

    fuse_unmount(local_mount_dir, ch);
  }
  return err ? 1 : 0;
}

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::init(void *userdata, struct fuse_conn_info *conn)
/* -------------------------------------------------------------------------- */
{

  eos_static_debug("");
}

void
EosFuse::destroy(void *userdata)
{

  eos_static_debug("");
}

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::DumpStatistic()
/* -------------------------------------------------------------------------- */
{
  eos_static_debug("started statistic dump thread");
  XrdSysTimer sleeper;
  char ino_stat[16384];
  while (1)
  {
    eos::common::LinuxMemConsumption::linux_mem_t mem;
    eos::common::LinuxStat::linux_stat_t osstat;

    if (!eos::common::LinuxMemConsumption::GetMemoryFootprint(mem))
    {
      eos_static_err("failed to get the MEM usage information");
    }


    if (!eos::common::LinuxStat::GetStat(osstat))
    {
      eos_static_err("failed to get the OS usage information");
    }

    eos_static_debug("dumping statistics");
    XrdOucString out;
    EosFuse::Instance().getFuseStat().PrintOutTotal(out);
    std::string sout = out.c_str();

    snprintf(ino_stat, sizeof (ino_stat),
             "# -----------------------------------------------------------------------------------------------------------\n"
             "ALL        inodes              := %lu\n"
             "ALL        inodes-todelete     := %lu\n"
             "ALL        inodes-backlog      := %lu\n"
             "ALL        inodes-ever         := %lu\n"
             "ALL        inodes-ever-deleted := %lu\n"
             "# -----------------------------------------------------------------------------------------------------------\n",
             EosFuse::Instance().getMdStat().inodes(),
             EosFuse::Instance().getMdStat().inodes_deleted(),
             EosFuse::Instance().getMdStat().inodes_backlog(),
             EosFuse::Instance().getMdStat().inodes_ever(),
             EosFuse::Instance().getMdStat().inodes_deleted_ever());

    sout += ino_stat;

    std::string s1;
    std::string s2;

    snprintf(ino_stat, sizeof (ino_stat),
             "ALL        threads             := %llu\n"
             "ALL        visze               := %s\n"
             "All        rss                 := %s\n"
             "# -----------------------------------------------------------------------------------------------------------\n",
             osstat.threads,
             eos::common::StringConversion::GetReadableSizeString(s1, osstat.vsize, "b"),
             eos::common::StringConversion::GetReadableSizeString(s2, osstat.rss, "b")
             );

    sout += ino_stat;

    std::ofstream dumpfile(EosFuse::Instance().config.statfilepath);
    dumpfile << sout;
    sleeper.Snooze(1);
  }
}

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::StatCirculate()
/* -------------------------------------------------------------------------- */
{
  eos_static_debug("started stat circulate thread");
  Stat::Instance().Circulate();
}

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
/* -------------------------------------------------------------------------- */
{
  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);

  eos_static_debug("");

  ADD_FUSE_STAT(__func__, req);

  EXEC_TIMING_BEGIN(__func__);

  int rc = 0;

  fuse_id id(req);

  metad::shared_md md = Instance().mds.get(req, ino);
  if (!md->id())
  {
    rc = ENOENT;
    fuse_reply_err(req, rc);
  }
  else
  {
    XrdSysMutexHelper mLock(md->Locker());
    struct fuse_entry_param e;
    md->convert(e);
    eos_static_info("%s", md->dump(e).c_str());
    fuse_reply_attr (req, &e.attr, e.attr_timeout);
  }

  EXEC_TIMING_END(__func__);

  COMMONTIMING("_stop_", &timing);
  eos_static_notice("t(ms)=%.03f %s", timing.RealTime(),
                    dump(id, ino, fi, rc).c_str());
}

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::setattr(fuse_req_t req, fuse_ino_t ino, struct stat *attr, int op,
                 struct fuse_file_info *fi)
/* -------------------------------------------------------------------------- */
{
  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);

  eos_static_debug("");

  ADD_FUSE_STAT(__func__, req);

  EXEC_TIMING_BEGIN(__func__);

  int rc = 0;

  fuse_id id(req);

  cap::shared_cap pcap;

  // retrieve the appropriate cap


  if (op & FUSE_SET_ATTR_MODE)
  {
    // retrieve cap for mode setting
    pcap = Instance().caps.acquire(req, ino,
                                   M_OK);
  }
  else
    if ((op & FUSE_SET_ATTR_UID) && (op & FUSE_SET_ATTR_GID))
  {
    // retrieve cap for owner setting
    pcap = Instance().caps.acquire(req, ino,
                                   C_OK);
  }
  else
    if (op & FUSE_SET_ATTR_SIZE)
  {
    // retrieve cap for write
    pcap = Instance().caps.acquire(req, ino,
                                   W_OK);
  }
  else
    if ( (op & FUSE_SET_ATTR_ATIME)
        || (op & FUSE_SET_ATTR_MTIME)
        || (op & FUSE_SET_ATTR_ATIME_NOW)
        || (op & FUSE_SET_ATTR_MTIME_NOW)
        )
  {
    // retrieve cap for write
    pcap = Instance().caps.acquire(req, ino,
                                   W_OK);
  }

  if (pcap->errc())
  {
    rc = pcap->errc();
  }
  else
  {
    if (!rc)
    {
      metad::shared_md md;

      md = Instance().mds.get(req, ino);

      {
        XrdSysMutexHelper mLock(md->Locker());

        if (!md->id() || md->deleted())
        {
          rc = ENOENT;
        }
      }
      if (!rc)
      {
        if (op & FUSE_SET_ATTR_MODE)
        {
          /*
            EACCES Search permission is denied on a component of the path prefix.  

            EFAULT path points outside your accessible address space.

            EIO    An I/O error occurred.

            ELOOP  Too many symbolic links were encountered in resolving path.

            ENAMETOOLONG
                   path is too long.

            ENOENT The file does not exist.

            ENOMEM Insufficient kernel memory was available.

            ENOTDIR
                   A component of the path prefix is not a directory.

            EPERM  The  effective  UID does not match the owner of the file, 
                   and the process is not privileged (Linux: it does not
           
                   have the CAP_FOWNER capability).

            EROFS  The named file resides on a read-only filesystem.

            The general errors for fchmod() are listed below:

            EBADF  The file descriptor fd is not valid.

            EIO    See above.

            EPERM  See above.

            EROFS  See above.
           */
          ADD_FUSE_STAT("setattr:chmod", req);

          EXEC_TIMING_BEGIN("setattr:chmod");

          XrdSysMutexHelper mLock(md->Locker());

          md->set_mode(attr->st_mode);

          Instance().mds.update(req, md);

          struct fuse_entry_param e;
          md->convert(e);
          eos_static_info("%s", md->dump(e).c_str());
          fuse_reply_attr (req, &e.attr, e.attr_timeout);

          EXEC_TIMING_END("setattr:chmod");
        }

        if ((op & FUSE_SET_ATTR_UID) && (op & FUSE_SET_ATTR_GID))
        {
          /*
            EACCES Search permission is denied on a component of the path prefix.  

            EFAULT path points outside your accessible address space.

            ELOOP  Too many symbolic links were encountered in resolving path.

            ENAMETOOLONG
                   path is too long.

            ENOENT The file does not exist.

            ENOMEM Insufficient kernel memory was available.

            ENOTDIR
                   A component of the path prefix is not a directory.

            EPERM  The calling process did not have the required permissions 
                   (see above) to change owner and/or group.

            EROFS  The named file resides on a read-only filesystem.

            The general errors for fchown() are listed below:

            EBADF  The descriptor is not valid.

            EIO    A low-level I/O error occurred while modifying the inode.

            ENOENT See above.

            EPERM  See above.

            EROFS  See above.
           */

          ADD_FUSE_STAT("setattr:chown", req);

          EXEC_TIMING_BEGIN("setattr:chown");

          XrdSysMutexHelper mLock(md->Locker());

          md->set_uid(attr->st_uid);
          md->set_gid(attr->st_gid);

          Instance().mds.update(req, md);

          struct fuse_entry_param e;
          md->convert(e);
          eos_static_info("%s", md->dump(e).c_str());
          fuse_reply_attr (req, &e.attr, e.attr_timeout);

          EXEC_TIMING_END("setattr:chown");

        }

        if (
            (op & FUSE_SET_ATTR_ATIME)
            || (op & FUSE_SET_ATTR_MTIME)
            || (op & FUSE_SET_ATTR_ATIME_NOW)
            || (op & FUSE_SET_ATTR_MTIME_NOW)
            )
        {
          /*
          EACCES Search permission is denied for one of the directories in 
     the  path  prefix  of  path

          EACCES times  is  NULL,  the caller's effective user ID does not match 
     the owner of the file, the caller does not have
     write access to the file, and the caller is not privileged 
     (Linux: does not have either the CAP_DAC_OVERRIDE or
     the CAP_FOWNER capability).

          ENOENT filename does not exist.

          EPERM  times is not NULL, the caller's effective UID does not 
     match the owner of the file, and the caller is not priv‐
     ileged (Linux: does not have the CAP_FOWNER capability).

          EROFS  path resides on a read-only filesystem.
           */

          ADD_FUSE_STAT("setattr:utimes", req);

          EXEC_TIMING_BEGIN("setattr:utimes");

          XrdSysMutexHelper mLock(md->Locker());

          if (op & FUSE_SET_ATTR_ATIME)
          {
            md->set_atime(attr->ATIMESPEC.tv_sec);
            md->set_atime_ns(attr->ATIMESPEC.tv_nsec);
          }
          if (op & FUSE_SET_ATTR_MTIME)
          {
            md->set_mtime(attr->MTIMESPEC.tv_sec);
            md->set_mtime_ns(attr->MTIMESPEC.tv_nsec);
          }

          if ( ( op & FUSE_SET_ATTR_ATIME_NOW) ||
              ( op & FUSE_SET_ATTR_MTIME_NOW) )
          {
            struct timespec tsnow;
            eos::common::Timing::GetTimeSpec(tsnow);

            if (op & FUSE_SET_ATTR_ATIME_NOW)
            {
              md->set_mtime(tsnow.tv_sec);
              md->set_mtime_ns(tsnow.tv_nsec);
            }

            if (op & FUSE_SET_ATTR_MTIME_NOW)
            {
              md->set_mtime(tsnow.tv_sec);
              md->set_mtime_ns(tsnow.tv_nsec);
            }
          }
          Instance().mds.update(req, md);

          struct fuse_entry_param e;
          md->convert(e);
          eos_static_info("%s", md->dump(e).c_str());
          fuse_reply_attr (req, &e.attr, e.attr_timeout);

          EXEC_TIMING_END("setattr:chown");
        }


        if (op & FUSE_SET_ATTR_SIZE)
        {
          /*
EACCES Search  permission is denied for a component of the path 
       prefix, or the named file is not writable by the user.
             
EFAULT Path points outside the process's allocated address space.

EFBIG  The argument length is larger than the maximum file size. 

EINTR  While blocked waiting to complete, the call was interrupted 
       by a signal handler; see fcntl(2) and signal(7).

EINVAL The argument length is negative or larger than the maximum 
       file size.

EIO    An I/O error occurred updating the inode.

EISDIR The named file is a directory.

ELOOP  Too many symbolic links were encountered in translating the 
       pathname.

ENAMETOOLONG
       A component of a pathname exceeded 255 characters, or an 
       entire pathname exceeded 1023 characters.

ENOENT The named file does not exist.

ENOTDIR
       A component of the path prefix is not a directory.

EPERM  The underlying filesystem does not support extending a file 
       beyond its current size.

EROFS  The named file resides on a read-only filesystem.

ETXTBSY
       The file is a pure procedure (shared text) file that is 
       being executed.

For ftruncate() the same errors apply, but instead of things that 
can be wrong with path, we now have things that  can
be wrong with the file descriptor, fd:

EBADF  fd is not a valid descriptor.

EBADF or EINVAL
       fd is not open for writing.

EINVAL fd does not reference a regular file.
           */

          ADD_FUSE_STAT("setattr:truncate", req);

          EXEC_TIMING_BEGIN("setattr:truncate");


          int rc = 0;

          // do a parent check
          cap::shared_cap pcap = Instance().caps.acquire(req, ino,
                                                         S_IFREG | W_OK);

          if (pcap->errc())
          {
            rc = pcap->errc();
          }
          else
          {
            metad::shared_md md;
            md = Instance().mds.get(req, ino);

            XrdSysMutexHelper mLock(md->Locker());

            if (!md->id() || md->deleted())
            {
              rc = ENOENT;
            }
            else
            {
              if ((md->mode() & S_IFDIR))
              {
                rc = EISDIR;
              }
              else
              {
                data::shared_data io = Instance().datas.get(req, md->id());
                rc = io->truncate(attr->st_size);

                if (!rc)
                {
                  md->set_size(attr->st_size);
                  Instance().mds.update(req, md);

                  struct fuse_entry_param e;
                  md->convert(e);
                  eos_static_info("%s", md->dump(e).c_str());
                  fuse_reply_attr (req, &e.attr, e.attr_timeout);
                }
              }
            }
          }
          EXEC_TIMING_END("setattr:truncate");
        }
      }
    }
  }

  if (rc)
    fuse_reply_err (req, rc);

  EXEC_TIMING_END(__func__);

  COMMONTIMING("_stop_", &timing);

  eos_static_notice("t(ms)=%.03f %s", timing.RealTime(),
                    dump(id, ino, fi, rc).c_str());
}

void
EosFuse::lookup(fuse_req_t req, fuse_ino_t parent, const char *name)
{
  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);

  eos_static_debug("");

  ADD_FUSE_STAT(__func__, req);

  EXEC_TIMING_BEGIN(__func__);

  int rc = 0;

  fuse_id id(req);

  struct fuse_entry_param e;
  memset(&e, 0, sizeof (e));
  {
    metad::shared_md md;
    md = Instance().mds.lookup(req, parent, name);

    if (md->id() && !md->deleted())
    {
      XrdSysMutexHelper mLock(md->Locker());
      md->convert(e);
      eos_static_info("%s", md->dump(e).c_str());
      md->lookup_inc();
    }
    else
    {
      // negative cache entry

      e.ino = 0;
      e.attr_timeout = 0;
      e.entry_timeout = 0;
      rc = ENOENT;

      // for the moment no negative stat cache
    }
  }

  EXEC_TIMING_END(__func__);

  COMMONTIMING("_stop_", &timing);
  eos_static_notice("t(ms)=%.03f name=%s %s", timing.RealTime(), name,
                    dump(id, parent, 0, rc).c_str());

  if (rc)
    fuse_reply_err (req, rc);
  else
    fuse_reply_entry(req, &e);
}

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::opendir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info * fi)
/* -------------------------------------------------------------------------- */
{
  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);

  eos_static_debug("");

  EXEC_TIMING_BEGIN(__func__);

  ADD_FUSE_STAT(__func__, req);

  int rc = 0;

  fuse_id id(req);

  // retrieve cap

  cap::shared_cap pcap = Instance().caps.acquire(req, ino,
                                                 S_IFDIR | X_OK | R_OK);

  if (pcap->errc())
  {
    rc = pcap->errc();
    fuse_reply_err (req, rc);
  }
  else
  {
    // retrieve md
    metad::shared_md md = Instance().mds.get(req, ino);

    XrdSysMutexHelper mLock(md->Locker());

    if (!md->id() || md->deleted())
    {
      fuse_reply_err(req, ENOENT);
    }
    else
    {
      eos_static_info("%s", md->dump().c_str());

      // copy the current state
      eos::fusex::md* fh_md = new eos::fusex::md(*md);
      fi->fh = (unsigned long) fh_md;
      fuse_reply_open (req, fi);
    }
  }

  EXEC_TIMING_END(__func__);

  COMMONTIMING("_stop_", &timing);
  eos_static_notice("t(ms)=%.03f %s", timing.RealTime(),
                    dump(id, ino, 0, rc).c_str());
}

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
                 struct fuse_file_info * fi)
/* -------------------------------------------------------------------------- */
/*
EBADF  Invalid directory stream descriptor fi->fh
 */
{
  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);

  EXEC_TIMING_BEGIN(__func__);

  ADD_FUSE_STAT(__func__, req);

  int rc = 0;

  fuse_id id(req);

  if (!fi->fh)
  {
    fuse_reply_err (req, EBADF);
    rc = EBADF;
  }
  else
  {
    eos::fusex::md* md = (eos::fusex::md*) fi->fh;
    auto map = md->children();
    auto it = map.begin();

    eos_static_info("off=%lu size=%lu", off, map.size());

    if ((size_t) off < map.size())
      std::advance(it, off);
    else
      it = map.end();

    char b[size];

    char* b_ptr = b;
    off_t b_size = 0;

    for ( ; it != map.end(); ++it)
    {
      std::string bname = it->first;
      fuse_ino_t cino = it->second;

      metad::shared_md cmd = Instance().mds.get(req, cino);

      mode_t mode = cmd->mode();

      struct stat stbuf;
      memset (&stbuf, 0, sizeof ( struct stat ));
      stbuf.st_ino = cino;
      stbuf.st_mode = mode;

      size_t a_size = fuse_add_direntry (req, b_ptr , size - b_size,
                                         bname.c_str(), &stbuf, ++off);

      eos_static_info("name=%s ino=%08lx mode=%08x bytes=%u/%u",
                      bname.c_str(), cino, mode, a_size, size - b_size);

      if (a_size > (size - b_size))
        break;

      b_ptr += a_size;
      b_size += a_size;
    }

    fuse_reply_buf (req, b_size ? b : 0, b_size);

    eos_static_debug("size=%lu off=%llu reply-size=%lu", size, off, b_size);
  }

  EXEC_TIMING_END(__func__);

  COMMONTIMING("_stop_", &timing);
  eos_static_notice("t(ms)=%.03f %s", timing.RealTime(),
                    dump(id, ino, 0, rc).c_str());
}

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::releasedir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info * fi)
/* -------------------------------------------------------------------------- */
{
  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);

  eos_static_debug("");

  EXEC_TIMING_BEGIN(__func__);

  ADD_FUSE_STAT(__func__, req);

  int rc = 0;

  fuse_id id(req);

  eos::fusex::md* md = (eos::fusex::md*) fi->fh;
  if (md)
  {
    delete md;
    fi->fh = 0;
  }

  EXEC_TIMING_END(__func__);

  fuse_reply_err(req, 0);

  COMMONTIMING("_stop_", &timing);
  eos_static_notice("t(ms)=%.03f %s", timing.RealTime(),
                    dump(id, ino, 0, rc).c_str());
}

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::statfs(fuse_req_t req, fuse_ino_t ino)
/* -------------------------------------------------------------------------- */
{
  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);

  eos_static_debug("");

  ADD_FUSE_STAT(__func__, req);

  EXEC_TIMING_BEGIN(__func__);

  int rc=0;

  fuse_id id(req);

  struct statvfs svfs;
  memset(&svfs, 0, sizeof (struct statvfs));

  svfs.f_bsize = 128 * 1024;
  svfs.f_blocks = 1000000000ll;
  svfs.f_bfree = 1000000000ll;
  svfs.f_bavail = 1000000000ll;
  svfs.f_files = 1000000;
  svfs.f_ffree = 1000000;

  if (rc)
    fuse_reply_err (req, rc);

  else
    fuse_reply_statfs (req, &svfs);

  EXEC_TIMING_END(__func__);

  COMMONTIMING("_stop_", &timing);
  eos_static_notice("t(ms)=%.03f %s", timing.RealTime(),
                    dump(id, ino, 0, rc).c_str());

  return;
}

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::mkdir(fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode)
/* -------------------------------------------------------------------------- */
/*
EACCES The parent directory does not allow write permission to the process, 
or one of the directories in pathname  did
  
not allow search permission.  (See also path_resolution(7).)

EDQUOT The user's quota of disk blocks or inodes on the filesystem has been 
exhausted.

EEXIST pathname  already exists (not necessarily as a directory).  
This includes the case where pathname is a symbolic
link, dangling or not.

EFAULT pathname points outside your accessible address space.

ELOOP  Too many symbolic links were encountered in resolving pathname.

EMLINK The number of links to the parent directory would exceed LINK_MAX.

ENAMETOOLONG
pathname was too long.

ENOENT A directory component in pathname does not exist or is a dangling 
symbolic link.

ENOMEM Insufficient kernel memory was available.

ENOSPC The device containing pathname has no room for the new directory.

ENOSPC The new directory cannot be created because the user's disk quota is 
exhausted.

ENOTDIR
A component used as a directory in pathname is not, in fact, a directory.

EPERM  The filesystem containing pathname does not support the creation of 
directories.

EROFS  pathname refers to a file on a read-only filesystem.
 */
/* -------------------------------------------------------------------------- */
{
  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);

  eos_static_debug("");

  ADD_FUSE_STAT(__func__, req);

  EXEC_TIMING_BEGIN(__func__);

  int rc = 0;

  fuse_id id(req);

  // do a parent check
  cap::shared_cap pcap = Instance().caps.acquire(req, parent,
                                                 S_IFDIR | X_OK | W_OK);


  if (pcap->errc())
  {
    rc = pcap->errc();
  }
  else
  {
    metad::shared_md md;
    metad::shared_md pmd;
    md = Instance().mds.lookup(req, parent, name);
    pmd = Instance().mds.get(req, parent);

    XrdSysMutexHelper mLock(md->Locker());
    if (md->id() && !md->deleted())
    {
      rc = EEXIST;
    }
    else
    {

      md->set_mode(mode | S_IFDIR);
      struct timespec ts;
      eos::common::Timing::GetTimeSpec(ts);
      md->set_name(name);
      md->set_atime(ts.tv_sec);
      md->set_atime_ns(ts.tv_nsec);
      md->set_mtime(ts.tv_sec);
      md->set_mtime_ns(ts.tv_sec);
      md->set_ctime(ts.tv_sec);
      md->set_ctime_ns(ts.tv_nsec);
      md->set_btime(ts.tv_sec);
      md->set_btime_ns(ts.tv_nsec);
      md->set_uid(pcap->uid());
      md->set_gid(pcap->gid());
      md->set_id(Instance().mds.insert(req, md));

      Instance().mds.add(pmd, md);

      struct fuse_entry_param e;
      memset(&e, 0, sizeof (e));
      md->convert(e);
      md->lookup_inc();
      fuse_reply_entry(req, &e);
      eos_static_info("%s", md->dump(e).c_str());
    }
  }

  if (rc)
    fuse_reply_err (req, rc);

  EXEC_TIMING_END(__func__);

  COMMONTIMING("_stop_", &timing);
  eos_static_notice("t(ms)=%.03f %s", timing.RealTime(),
                    dump(id, parent, 0, rc).c_str());
}

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::unlink(fuse_req_t req, fuse_ino_t parent, const char *name)
/* -------------------------------------------------------------------------- */
/*
EACCES Write access to the directory containing pathname is not allowed for the process's effective UID, or one of the
directories in pathname did not allow search permission.  (See also path_resolution(7).)

EBUSY  The file pathname cannot be unlinked because it is being used by the system or another process; for example, it
is a mount point or the NFS client software created it to represent an  active  but  otherwise  nameless  inode
("NFS silly renamed").

EFAULT pathname points outside your accessible address space.

EIO    An I/O error occurred.

EISDIR pathname refers to a directory.  (This is the non-POSIX value returned by Linux since 2.1.132.)

ELOOP  Too many symbolic links were encountered in translating pathname.

ENAMETOOLONG
pathname was too long.

ENOENT A component in pathname does not exist or is a dangling symbolic link, or pathname is empty.

ENOMEM Insufficient kernel memory was available.

ENOTDIR
A component used as a directory in pathname is not, in fact, a directory.

EPERM  The  system  does  not allow unlinking of directories, or unlinking of directories requires privileges that the
calling process doesn't have.  (This is the POSIX prescribed error return; as noted above, Linux returns EISDIR
for this case.)

EPERM (Linux only)
The filesystem does not allow unlinking of files.

EPERM or EACCES
The  directory  containing pathname has the sticky bit (S_ISVTX) set and the process's effective UID is neither
the UID of the file to be deleted nor that of the directory containing it, and the process  is  not  privileged
(Linux: does not have the CAP_FOWNER capability).

EROFS  pathname refers to a file on a read-only filesystem.

 */
{

  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);

  eos_static_debug("");

  ADD_FUSE_STAT(__func__, req);

  EXEC_TIMING_BEGIN(__func__);

  int rc = 0;

  fuse_id id(req);

  // retrieve cap

  cap::shared_cap pcap = Instance().caps.acquire(req, parent,
                                                 S_IFDIR | X_OK | D_OK);

  if (pcap->errc())
  {
    rc = pcap->errc();

  }
  else
  {
    std::string sname = name;

    if (sname == ".")
    {
      rc = EINVAL;
    }

    if (sname.length() > 1024)
    {
      rc = ENAMETOOLONG;
    }

    if (!rc)
    {
      metad::shared_md md;
      metad::shared_md pmd;

      md = Instance().mds.lookup(req, parent, name);

      XrdSysMutexHelper mLock(md->Locker());

      if (!md->id() || md->deleted())
      {
        rc = ENOENT;
      }

      if ((!rc) && ( (md->mode() & S_IFDIR)))
      {
        rc = EISDIR;
      }

      if (!rc)
      {
        pmd = Instance().mds.get(req, parent);
        Instance().mds.remove(pmd, md);
        Instance().datas.unlink(md->id());
      }
    }
  }

  fuse_reply_err (req, rc);

  EXEC_TIMING_END(__func__);

  COMMONTIMING("_stop_", &timing);
  eos_static_notice("t(ms)=%.03f %s", timing.RealTime(),
                    dump(id, parent, 0, rc).c_str());
}

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::rmdir(fuse_req_t req, fuse_ino_t parent, const char * name)
/* -------------------------------------------------------------------------- */
/*
EACCES Write access to the directory containing pathname was not allowed, 
or one of the directories in the path prefix
of pathname did not allow search permission.  

EBUSY  pathname is currently in use by the system or some process that 
prevents its  removal.   On  Linux  this  means
pathname is currently used as a mount point or is the root directory of
the calling process.

EFAULT pathname points outside your accessible address space.

EINVAL pathname has .  as last component.

ELOOP  Too many symbolic links were encountered in resolving pathname.

ENAMETOOLONG
pathname was too long.

ENOENT A directory component in pathname does not exist or is a dangling 
symbolic link.

ENOMEM Insufficient kernel memory was available.

ENOTDIR
pathname, or a component used as a directory in pathname, is not, 
in fact, a directory.

ENOTEMPTY
pathname contains entries other than . and .. ; or, pathname has ..  
as its final component.  POSIX.1-2001 also
allows EEXIST for this condition.

EPERM  The directory containing pathname has the sticky bit (S_ISVTX) set and 
the process's effective user ID is  nei‐
ther  the  user  ID  of  the file to be deleted nor that of the 
directory containing it, and the process is not
privileged (Linux: does not have the CAP_FOWNER capability).

EPERM  The filesystem containing pathname does not support the removal of 
directories.

EROFS  pathname refers to a directory on a read-only filesystem.

 */
{
  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);

  eos_static_debug("");

  ADD_FUSE_STAT(__func__, req);

  EXEC_TIMING_BEGIN(__func__);

  int rc = 0;

  fuse_id id(req);

  // retrieve cap

  cap::shared_cap pcap = Instance().caps.acquire(req, parent,
                                                 S_IFDIR | X_OK | D_OK);

  if (pcap->errc())
  {
    rc = pcap->errc();
  }
  else
  {
    std::string sname = name;

    if (sname == ".")
    {
      rc = EINVAL;
    }

    if (sname.length() > 1024)
    {
      rc = ENAMETOOLONG;
    }

    if (!rc)
    {
      metad::shared_md md;
      metad::shared_md pmd;

      md = Instance().mds.lookup(req, parent, name);

      XrdSysMutexHelper mLock(md->Locker());

      if (!md->id() || md->deleted())
      {
        rc = ENOENT;
      }

      if ((!rc) && (! (md->mode() & S_IFDIR)))
      {
        rc = ENOTDIR;
      }

      if ((!rc) && md->children().size())
      {
        ;
        rc = ENOTEMPTY;
      }

      if (!rc)
      {

        pmd = Instance().mds.get(req, parent);
        Instance().mds.remove(pmd, md);
      }
    }
  }

  fuse_reply_err (req, rc);

  EXEC_TIMING_END(__func__);

  COMMONTIMING("_stop_", &timing);
  eos_static_notice("t(ms)=%.03f %s", timing.RealTime(),
                    dump(id, parent, 0, rc).c_str());
}

#ifdef _FUSE3
/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::rename(fuse_req_t req, fuse_ino_t parent, const char *name,
                fuse_ino_t newparent, const char *newname, unsigned int flags)
/* -------------------------------------------------------------------------- */
#else

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::rename(fuse_req_t req, fuse_ino_t parent, const char *name,
                fuse_ino_t newparent, const char *newname)
/* -------------------------------------------------------------------------- */
#endif
{

  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);

  eos_static_debug("");

  ADD_FUSE_STAT(__func__, req);

  EXEC_TIMING_BEGIN(__func__);

  EXEC_TIMING_END(__func__);

  COMMONTIMING("_stop_", &timing);
  eos_static_notice("RT %-16s %.04f", __FUNCTION__, timing.RealTime());
}

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::access(fuse_req_t req, fuse_ino_t ino, int mask)
/* -------------------------------------------------------------------------- */
{

  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);

  eos_static_debug("");

  EXEC_TIMING_BEGIN(__func__);

  fuse_reply_err(req, 0);

  EXEC_TIMING_END(__func__);

  COMMONTIMING("_stop_", &timing);
  eos_static_notice("RT %-16s %.04f", __FUNCTION__, timing.RealTime());
}

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info * fi)
/* -------------------------------------------------------------------------- */
{

  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);

  eos_static_debug("");

  ADD_FUSE_STAT(__func__, req);

  EXEC_TIMING_BEGIN(__func__);

  int rc = 0;

  fuse_id id(req);

  int mode = R_OK;

  if (fi->flags & (O_RDWR | O_WRONLY) )
  {
    mode = W_OK;
  }

  // do a parent check
  cap::shared_cap pcap = Instance().caps.acquire(req, ino,
                                                 S_IFREG | mode);

  if (pcap->errc())
  {
    rc = pcap->errc();
  }
  else
  {
    metad::shared_md md;
    md = Instance().mds.get(req, ino);

    XrdSysMutexHelper mLock(md->Locker());

    if (!md->id() || md->deleted())
    {
      rc = ENOENT;
    }
    else
    {
      struct fuse_entry_param e;
      memset(&e, 0, sizeof (e));
      md->convert(e);

      data::data_fh* io = data::data_fh::Instance(Instance().datas.get(req, md->id()), md);
      // attach a datapool object
      fi->fh = (uint64_t) io;

      io->ioctx()->attach();

      fi->keep_cache = 0;
      fi->direct_io = 0;
      fuse_reply_open(req, fi);
      eos_static_info("%s", md->dump(e).c_str());
    }
  }

  if (rc)
    fuse_reply_err (req, rc);
  EXEC_TIMING_END(__func__);

  COMMONTIMING("_stop_", &timing);

  eos_static_notice("t(ms)=%.03f %s", timing.RealTime(),
                    dump(id, ino, fi, rc).c_str());

}

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::mknod(fuse_req_t req, fuse_ino_t parent, const char *name,
               mode_t mode, dev_t rdev)
/* -------------------------------------------------------------------------- */
{

  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);

  eos_static_debug("");

  ADD_FUSE_STAT(__func__, req);

  EXEC_TIMING_BEGIN(__func__);

  EXEC_TIMING_END(__func__);

  COMMONTIMING("_stop_", &timing);
  eos_static_notice("RT %-16s %.04f", __FUNCTION__, timing.RealTime());
}

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::create(fuse_req_t req, fuse_ino_t parent, const char *name,
                mode_t mode, struct fuse_file_info * fi)
/* -------------------------------------------------------------------------- */
/*
EACCES The  requested  access to the file is not allowed, or search permission is denied for one of the directories in
the path prefix of pathname, or the file did not exist yet and write access to  the  parent  directory  is  not
allowed.  (See also path_resolution(7).)

EDQUOT Where  O_CREAT  is  specified,  the  file  does not exist, and the user's quota of disk blocks or inodes on the
filesystem has been exhausted.

EEXIST pathname already exists and O_CREAT and O_EXCL were used.

EFAULT pathname points outside your accessible address space.

EFBIG  See EOVERFLOW.

EINTR  While blocked waiting to complete an open of a slow device (e.g., a FIFO; see fifo(7)),  the  call  was  inter‐
rupted by a signal handler; see signal(7).

EINVAL The filesystem does not support the O_DIRECT flag. See NOTES for more information.

EISDIR pathname refers to a directory and the access requested involved writing (that is, O_WRONLY or O_RDWR is set).

ELOOP  Too  many symbolic links were encountered in resolving pathname, or O_NOFOLLOW was specified but pathname was a
symbolic link.

EMFILE The process already has the maximum number of files open.

ENAMETOOLONG
pathname was too long.

ENFILE The system limit on the total number of open files has been reached.

ENODEV pathname refers to a device special file and no corresponding device exists.  (This is a Linux kernel  bug;  in
this situation ENXIO must be returned.)

ENOENT O_CREAT  is not set and the named file does not exist.  Or, a directory component in pathname does not exist or
is a dangling symbolic link.

ENOMEM Insufficient kernel memory was available.

ENOSPC pathname was to be created but the device containing pathname has no room for the new file.

ENOTDIR
A component used as a directory in pathname is not, in fact, a directory,  or  O_DIRECTORY  was  specified  and
pathname was not a directory.

ENXIO  O_NONBLOCK  |  O_WRONLY is set, the named file is a FIFO and no process has the file open for reading.  Or, the
file is a device special file and no corresponding device exists.

EOVERFLOW
pathname refers to a regular file that is too large to be opened.  The usual scenario here is that an  applica‐
tion  compiled  on  a  32-bit  platform  without -D_FILE_OFFSET_BITS=64 tried to open a file whose size exceeds
(2<<31)-1 bits; see also O_LARGEFILE above.  This is the error specified by  POSIX.1-2001;  in  kernels  before
2.6.24, Linux gave the error EFBIG for this case.

EPERM  The  O_NOATIME  flag was specified, but the effective user ID of the caller did not match the owner of the file
and the caller was not privileged (CAP_FOWNER).

EROFS  pathname refers to a file on a read-only filesystem and write access was requested.

ETXTBSY
pathname refers to an executable image which is currently being executed and write access was requested.

EWOULDBLOCK
The O_NONBLOCK flag was specified, and an incompatible lease was held on the file (see fcntl(2)).
 */
/* -------------------------------------------------------------------------- */
{
  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);

  eos_static_debug("");

  ADD_FUSE_STAT(__func__, req);

  EXEC_TIMING_BEGIN(__func__);

  int rc = 0;

  fuse_id id(req);

  // do a parent check
  cap::shared_cap pcap = Instance().caps.acquire(req, parent,
                                                 S_IFDIR | W_OK);

  if (pcap->errc())
  {
    rc = pcap->errc();
  }
  else
  {
    metad::shared_md md;
    metad::shared_md pmd;
    md = Instance().mds.lookup(req, parent, name);
    pmd = Instance().mds.get(req, parent);

    XrdSysMutexHelper mLock(md->Locker());

    if (md->id() && !md->deleted())
    {
      rc = EEXIST;
    }
    else
    {
      md->set_mode(mode | S_IFREG);
      struct timespec ts;
      eos::common::Timing::GetTimeSpec(ts);
      md->set_name(name);
      md->set_atime(ts.tv_sec);
      md->set_atime_ns(ts.tv_nsec);
      md->set_mtime(ts.tv_sec);
      md->set_mtime_ns(ts.tv_sec);
      md->set_ctime(ts.tv_sec);
      md->set_ctime_ns(ts.tv_nsec);
      md->set_btime(ts.tv_sec);
      md->set_btime_ns(ts.tv_nsec);
      md->set_uid(pcap->uid());
      md->set_gid(pcap->gid());
      md->set_id(Instance().mds.insert(req, md));

      Instance().mds.add(pmd, md);

      struct fuse_entry_param e;
      memset(&e, 0, sizeof (e));
      md->convert(e);
      md->lookup_inc();
      // -----------------------------------------------------------------------
      // TODO: for the moment there is no kernel cache used until 
      // we add top-level management on it
      // -----------------------------------------------------------------------
      // FUSE caches the file for reads on the same filedescriptor in the buffer
      // cache, but the pages are released once this filedescriptor is released.
      fi->keep_cache = 0;

      if ( (fi->flags & O_DIRECT) ||
          (fi->flags & O_SYNC) )
        fi->direct_io = 1;

      else
        fi->direct_io = 0;

      data::data_fh* io = data::data_fh::Instance(Instance().datas.get(req, md->id()), md);

      // attach a datapool object
      fi->fh = (uint64_t) io;

      io->ioctx()->attach();

      fuse_reply_create(req, &e, fi);
      eos_static_info("%s", md->dump(e).c_str());
    }
  }

  if (rc)
    fuse_reply_err (req, rc);

  EXEC_TIMING_END(__func__);

  COMMONTIMING("_stop_", &timing);
  eos_static_notice("t(ms)=%.03f %s", timing.RealTime(),
                    dump(id, parent, 0, rc).c_str());
}

void
/* -------------------------------------------------------------------------- */
EosFuse::read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
              struct fuse_file_info * fi)
/* -------------------------------------------------------------------------- */
{

  eos_static_debug("inode=%llu size=%li off=%llu",
                   (unsigned long long) ino, size, (unsigned long long) off);

  eos_static_debug("");

  ADD_FUSE_STAT(__func__, req);

  EXEC_TIMING_BEGIN(__func__);

  data::data_fh* io = (data::data_fh*) fi->fh;
  ssize_t res=0;

  int rc = 0;

  if (io)
  {
    char* buf=0;
    if ( (res = io->ioctx()->peek_pread(buf, size, off)) == -1)
    {
      rc = EIO;
    }
    else
    {
      fuse_reply_buf (req, buf, res);
    }
    io->ioctx()->release_pread();
  }
  else
  {
    rc = ENXIO;
  }

  if (rc)
    fuse_reply_err (req, rc);

  EXEC_TIMING_END(__func__);

}

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::write(fuse_req_t req, fuse_ino_t ino, const char *buf, size_t size,
               off_t off, struct fuse_file_info * fi)
/* -------------------------------------------------------------------------- */
{

  eos_static_debug("inode=%lld size=%lld off=%lld buf=%lld",
                   (long long) ino, (long long) size,
                   (long long) off, (long long) buf);

  eos_static_debug("");

  ADD_FUSE_STAT(__func__, req);

  EXEC_TIMING_BEGIN(__func__);

  data::data_fh* io = (data::data_fh*) fi->fh;

  int rc = 0;

  if (io)
  {
    if (io->ioctx()->pwrite(buf, size, off) == -1)
    {
      rc = EIO;
    }
    else
    {
      {
        XrdSysMutexHelper mLock(io->mdctx()->Locker());
        io->mdctx()->set_size(io->ioctx()->size());
        io->set_update();
      }
      fuse_reply_write (req, size);
    }
  }
  else
  {
    rc = ENXIO;
  }

  if (rc)
    fuse_reply_err (req, rc);

  EXEC_TIMING_END(__func__);
}

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info * fi)
/* -------------------------------------------------------------------------- */
{

  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);

  eos_static_debug("");

  ADD_FUSE_STAT(__func__, req);

  EXEC_TIMING_BEGIN(__func__);

  int rc = 0;

  fuse_id id(req);

  if (fi->fh)
  {

    data::data_fh* io = (data::data_fh*) fi->fh;
    io->ioctx()->detach();
    delete io;

  }
  EXEC_TIMING_END(__func__);

  COMMONTIMING("_stop_", &timing);

  fuse_reply_err (req, rc);

  eos_static_notice("t(ms)=%.03f %s", timing.RealTime(),
                    dump(id, ino, 0, rc).c_str());
}

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::fsync(fuse_req_t req, fuse_ino_t ino, int datasync,
               struct fuse_file_info * fi)
/* -------------------------------------------------------------------------- */
{

  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);

  eos_static_debug("");

  ADD_FUSE_STAT(__func__, req);

  EXEC_TIMING_BEGIN(__func__);

  int rc = 0;

  fuse_id id(req);

  fuse_reply_err (req, rc);

  EXEC_TIMING_END(__func__);

  COMMONTIMING("_stop_", &timing);
  eos_static_notice("t(ms)=%.03f %s", timing.RealTime(),
                    dump(id, ino, 0, rc).c_str());
}

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::forget(fuse_req_t req, fuse_ino_t ino, unsigned long nlookup)
/* -------------------------------------------------------------------------- */
{

  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);

  eos_static_debug("");

  ADD_FUSE_STAT(__func__, req);

  EXEC_TIMING_BEGIN(__func__);

  int rc = 0;

  fuse_id id(req);

  rc = Instance().mds.forget(req, ino, nlookup);

  EXEC_TIMING_END(__func__);

  COMMONTIMING("_stop_", &timing);
  eos_static_notice("t(ms)=%.03f %s", timing.RealTime(),
                    dump(id, ino, 0, rc).c_str());
}

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::flush(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info * fi)
/* -------------------------------------------------------------------------- */
{

  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);

  eos_static_debug("");

  ADD_FUSE_STAT(__func__, req);

  EXEC_TIMING_BEGIN(__func__);

  int rc = 0;

  fuse_id id(req);

  data::data_fh* io = (data::data_fh*) fi->fh;

  if (io)
  {
    if (io->has_update())
    {
      struct timespec tsnow;
      eos::common::Timing::GetTimeSpec(tsnow);

      XrdSysMutexHelper mLock(io->md->Locker());
      Instance().mds.update(req, io->md);
      io->md->set_mtime(tsnow.tv_sec);
      io->md->set_mtime_ns(tsnow.tv_nsec);
    }
  }


  fuse_reply_err (req, rc);

  EXEC_TIMING_END(__func__);

  COMMONTIMING("_stop_", &timing);
  eos_static_notice("t(ms)=%.03f %s", timing.RealTime(),
                    dump(id, ino, 0, rc).c_str());
}

#ifdef __APPLE__
/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::getxattr(fuse_req_t req, fuse_ino_t ino, const char *xattr_name,
                  size_t size, uint32_t position)
/* -------------------------------------------------------------------------- */
#else

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::getxattr(fuse_req_t req, fuse_ino_t ino, const char *xattr_name,
                  size_t size)
/* -------------------------------------------------------------------------- */
#endif
{

  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);

  eos_static_debug("");

  ADD_FUSE_STAT(__func__, req);

  EXEC_TIMING_BEGIN(__func__);

  int rc = 0;

  fuse_id id(req);

  cap::shared_cap pcap;

  // retrieve the appropriate cap
  pcap = Instance().caps.acquire(req, ino,
                                 R_OK);

  if (pcap->errc())
  {
    rc = pcap->errc();
  }
  else
  {
    metad::shared_md md;

    md = Instance().mds.get(req, ino);

    XrdSysMutexHelper mLock(md->Locker());

    if (!md->id() || md->deleted())
    {
      rc = ENOENT;
    }
    else
    {
      auto map = md->attr();

      std::string key = xattr_name;
      static std::string s_sec = "security.";
      static std::string s_acl = "system.posix_acl";
      static std::string s_apple = "com.apple";

      // don't return any security attribute
      if (key.substr(0, s_sec.length()) == s_sec)
      {
        rc = ENOATTR;
      }
      else
        // don't return any posix acl attribute
        if (key == s_acl)
      {
        rc = ENOATTR;
      }
#ifdef __APPLE__
      else
        // don't return any finder attribute
        if (key.substr(0, s_apple.length()) == s_apple)
      {
        rc = ENOATTR;
      }
#endif
      else
      {
        if (!map.count(key))
        {
          rc = ENOATTR;
        }
        else
        {
          std::string value = map[key];


          if (size == 0 )
          {
            fuse_reply_xattr (req, value.size());
          }
          else
          {
            if (value.size() > size)
            {
              rc = ERANGE;
            }
            else
            {
              fuse_reply_buf (req, value.c_str(), value.size());
            }
          }
        }
      }
    }
  }

  if (rc)
    fuse_reply_err (req, rc);

  EXEC_TIMING_END(__func__);

  COMMONTIMING("_stop_", &timing);
  eos_static_notice("t(ms)=%.03f %s", timing.RealTime(),
                    dump(id, ino, 0, rc).c_str());
}

#ifdef __APPLE__
/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::setxattr(fuse_req_t req, fuse_ino_t ino, const char *xattr_name,
                  const char *xattr_value, size_t size, int flags,
                  uint32_t position)
/* -------------------------------------------------------------------------- */
#else

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::setxattr(fuse_req_t req, fuse_ino_t ino, const char *xattr_name,
                  const char *xattr_value, size_t size, int flags)
/* -------------------------------------------------------------------------- */
#endif
{

  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);

  eos_static_debug("");

  ADD_FUSE_STAT(__func__, req);

  EXEC_TIMING_BEGIN(__func__);

  int rc = 0;

  fuse_id id(req);

  cap::shared_cap pcap;

  // retrieve the appropriate cap
  pcap = Instance().caps.acquire(req, ino,
                                 SA_OK);

  if (pcap->errc())
  {
    rc = pcap->errc();
  }
  else
  {

    metad::shared_md md;

    md = Instance().mds.get(req, ino);

    XrdSysMutexHelper mLock(md->Locker());

    if (!md->id() || md->deleted())
    {
      rc = ENOENT;
    }
    else
    {
      std::string key = xattr_name;
      std::string value;
      value.assign(xattr_value, size);

      static std::string s_sec = "security.";
      static std::string s_acl = "system.posix_acl";
      static std::string s_apple = "com.apple";

      // ignore silently any security attribute
      if (key.substr(0, s_sec.length()) == s_sec)
      {
        rc = 0;
      }
      else
        // ignore silently any posix acl attribute
        if (key == s_acl)
      {
        rc = 0;
      }
#ifdef __APPLE__
      else
        // ignore silently any finder attribute
        if (key.substr(0, s_apple.length()) == s_apple)
      {
        rc = 0;
      }
#endif
      else
      {
        auto map = md->mutable_attr();

        bool exists=false;
        if ( (*map).count(key))
        {
          exists = true;
        }

        if (exists && (flags == XATTR_CREATE) )
        {
          rc = EEXIST;
        }
        else
          if ( !exists && (flags == XATTR_REPLACE) )
        {
          rc = ENOATTR;
        }
        else
        {
          (

                  *map)[key] = value;
          Instance().mds.update(req, md);
        }
      }
    }
  }

  fuse_reply_err (req, rc);

  EXEC_TIMING_END(__func__);

  COMMONTIMING("_stop_", &timing);
  eos_static_notice("t(ms)=%.03f %s", timing.RealTime(),
                    dump(id, ino, 0, rc).c_str());

}

/* -------------------------------------------------------------------------- */
void
/* -------------------------------------------------------------------------- */
EosFuse::listxattr(fuse_req_t req, fuse_ino_t ino, size_t size)
/* -------------------------------------------------------------------------- */
{

  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);

  eos_static_debug("");

  ADD_FUSE_STAT(__func__, req);

  EXEC_TIMING_BEGIN(__func__);

  int rc = 0;

  fuse_id id(req);

  cap::shared_cap pcap;

  // retrieve the appropriate cap
  pcap = Instance().caps.acquire(req, ino,
                                 R_OK);

  if (pcap->errc())
  {
    rc = pcap->errc();
  }
  else
  {

    metad::shared_md md;

    md = Instance().mds.get(req, ino);

    XrdSysMutexHelper mLock(md->Locker());

    if (!md->id() || md->deleted())
    {
      rc = ENOENT;
    }
    else
    {
      auto map = md->attr();

      size_t attrlistsize=0;
      std::string attrlist;

      for ( auto it = map.begin(); it != map.end(); ++it)
      {
        attrlistsize += it->first.length() + 1;
        attrlist += it->first;
        attrlist += '\0';
      }

      if (size == 0 )
      {
        fuse_reply_xattr (req, attrlistsize);
      }
      else
      {
        if (attrlist.size() > size)
        {
          rc = ERANGE;
        }
        else
        {
          fuse_reply_buf (req, attrlist.c_str(), attrlist.length());
        }
      }
    }
  }

  if (rc)
    fuse_reply_err (req, rc);

  EXEC_TIMING_END(__func__);

  COMMONTIMING("_stop_", &timing);
  eos_static_notice("t(ms)=%.03f %s", timing.RealTime(),
                    dump(id, ino, 0, rc).c_str());
}

void
/* -------------------------------------------------------------------------- */
EosFuse::removexattr(fuse_req_t req, fuse_ino_t ino, const char *xattr_name)
/* -------------------------------------------------------------------------- */
{

  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);

  eos_static_debug("");

  ADD_FUSE_STAT(__func__, req);

  EXEC_TIMING_BEGIN(__func__);

  int rc = 0 ;

  fuse_id id(req);

  cap::shared_cap pcap;

  // retrieve the appropriate cap
  pcap = Instance().caps.acquire(req, ino,
                                 SA_OK);

  if (pcap->errc())
  {
    rc = pcap->errc();
  }
  else
  {
    metad::shared_md md;

    md = Instance().mds.get(req, ino);

    XrdSysMutexHelper mLock(md->Locker());

    if (!md->id() || md->deleted())
    {
      rc = ENOENT;
    }
    else
    {
      std::string key = xattr_name;

      static std::string s_sec = "security.";
      static std::string s_acl = "system.posix_acl";
      static std::string s_apple = "com.apple";

      // ignore silently any security attribute
      if (key.substr(0, s_sec.length()) == s_sec)
      {
        rc = 0;
      }
      else
        // ignore silently any posix acl attribute
        if (key == s_acl)
      {
        rc = 0;
      }
#ifdef __APPLE__
      else
        // ignore silently any finder attribute
        if (key.substr(0, s_apple.length()) == s_apple)
      {
        rc = 0;
      }
#endif
      else
      {
        auto map = md->mutable_attr();

        bool exists=false;
        if ( (*map).count(key))
        {
          exists = true;
        }

        if ( !exists )
        {
          rc = ENOATTR;
        }
        else
        {
          (*map).erase(key);
          Instance().mds.update(req, md);
        }
      }
    }
  }

  fuse_reply_err (req, rc);

  EXEC_TIMING_END(__func__);

  COMMONTIMING("_stop_", &timing);
  eos_static_notice("t(ms)=%.03f %s", timing.RealTime(),
                    dump(id, ino, 0, rc).c_str());
}

void
/* -------------------------------------------------------------------------- */
EosFuse::readlink(fuse_req_t req, fuse_ino_t ino)
/* -------------------------------------------------------------------------- */
/*
 EACCES Search permission is denied for a component of the path prefix.  (See also path_resolution(7).)

 EFAULT buf extends outside the process’s allocated address space.

 EINVAL bufsiz is not positive.

 EINVAL The named file is not a symbolic link.

 EIO    An I/O error occurred while reading from the file system.

 ELOOP  Too many symbolic links were encountered in translating the pathname.

 ENAMETOOLONG
        A pathname, or a component of a pathname, was too long.

 ENOENT The named file does not exist.

 ENOMEM Insufficient kernel memory was available.

 ENOTDIR
        A component of the path prefix is not a directory.
 */
{

  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);

  eos_static_debug("");

  ADD_FUSE_STAT(__func__, req);

  EXEC_TIMING_BEGIN(__func__);

  int rc = 0;
  std::string target;

  fuse_id id(req);

  cap::shared_cap pcap;

  // retrieve the appropriate cap
  pcap = Instance().caps.acquire(req, ino,
                                 R_OK);

  if (pcap->errc())
  {
    rc = pcap->errc();
  }
  else
  {
    metad::shared_md md;

    md = Instance().mds.get(req, ino);

    XrdSysMutexHelper mLock(md->Locker());

    if (!md->id() || md->deleted())
    {
      rc = ENOENT;
    }
    else
    {
      if (! md->mode() & S_IFLNK)
      {
        // no a link
        rc = EINVAL;
      }
      else
      {
        target = md->target();
      }
    }
  }

  if (!rc)
  {
    fuse_reply_readlink (req, target.c_str());
    return;
  }
  else
  {
    fuse_reply_err (req, errno);
    return;
  }

  EXEC_TIMING_END(__func__);

  COMMONTIMING("_stop_", &timing);
  eos_static_notice("t(ms)=%.03f %s", timing.RealTime(),
                    dump(id, ino, 0, rc).c_str());
}

void
/* -------------------------------------------------------------------------- */
EosFuse::symlink(fuse_req_t req, const char *link, fuse_ino_t parent,
                 const char *name)
/* -------------------------------------------------------------------------- */
/*
 EACCES Write access to the directory containing newpath is denied, or one of the directories in the path
        prefix of newpath did not allow search permission.  (See also path_resolution(7).)

 EEXIST newpath already exists.

 EFAULT oldpath or newpath points outside your accessible address space.

 EIO    An I/O error occurred.

 ELOOP  Too many symbolic links were encountered in resolving newpath.

 ENAMETOOLONG
        oldpath or newpath was too long.

 ENOENT A directory component in newpath does not exist or is a dangling symbolic link, or oldpath is the
        empty string.

 ENOMEM Insufficient kernel memory was available.

 ENOSPC The device containing the file has no room for the new directory entry.

 ENOTDIR
        A component used as a directory in newpath is not, in fact, a directory.

 EPERM  The file system containing newpath does not support the creation of symbolic links.

 EROFS  newpath is on a read-only file system.

 */
{
  eos::common::Timing timing(__func__);
  COMMONTIMING("_start_", &timing);

  eos_static_debug("");

  ADD_FUSE_STAT(__func__, req);

  EXEC_TIMING_BEGIN(__func__);

  int rc = 0;

  fuse_id id(req);

  // do a parent check
  cap::shared_cap pcap = Instance().caps.acquire(req, parent,
                                                 S_IFDIR | W_OK);

  if (pcap->errc())
  {
    rc = pcap->errc();
  }
  else
  {
    metad::shared_md md;
    metad::shared_md pmd;
    md = Instance().mds.lookup(req, parent, name);
    pmd = Instance().mds.get(req, parent);

    XrdSysMutexHelper mLock(md->Locker());

    if (md->id() && !md->deleted())
    {
      rc = EEXIST;
    }
    else
    {
      md->set_mode( S_IRWXU | S_IRWXG | S_IRWXO | S_IFLNK );
      md->set_target(link);
      
      struct timespec ts;
      eos::common::Timing::GetTimeSpec(ts);
      md->set_name(name);
      md->set_atime(ts.tv_sec);
      md->set_atime_ns(ts.tv_nsec);
      md->set_mtime(ts.tv_sec);
      md->set_mtime_ns(ts.tv_sec);
      md->set_ctime(ts.tv_sec);
      md->set_ctime_ns(ts.tv_nsec);
      md->set_btime(ts.tv_sec);
      md->set_btime_ns(ts.tv_nsec);
      md->set_uid(pcap->uid());
      md->set_gid(pcap->gid());
      md->set_id(Instance().mds.insert(req, md));

      Instance().mds.add(pmd, md);

      struct fuse_entry_param e;
      memset(&e, 0, sizeof (e));
      md->convert(e);
      fuse_reply_entry (req, &e);
    }
  }

  if (rc)
  {
    fuse_reply_err (req, rc);
  }

  EXEC_TIMING_END(__func__);

  COMMONTIMING("_stop_", &timing);
  eos_static_notice("t(ms)=%.03f %s", timing.RealTime(),
                    dump(id, parent, 0, rc).c_str());
}