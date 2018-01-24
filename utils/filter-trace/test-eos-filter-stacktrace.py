################################################################################
## Unit tests for eos-filter-stacktrace.py                                    ##
## Author: Georgios Bitzes - CERN                                             ##
##                                                                            ##
## Copyright (C) 2018 CERN/Switzerland                                        ##
## This program is free software: you can redistribute it and/or modify       ##
## it under the terms of the GNU General Public License as published by       ##
## the Free Software Foundation, either version 3 of the License, or          ##
## (at your option) any later version.                                        ##
##                                                                            ##
## This program is distributed in the hope that it will be useful,            ##
## but WITHOUT ANY WARRANTY; without even the implied warranty of             ##
## MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the              ##
## GNU General Public License for more details.                               ##
##                                                                            ##
## You should have received a copy of the GNU General Public License          ##
## along with this program.  If not, see <http://www.gnu.org/licenses/>.      ##
################################################################################

from __future__ import absolute_import
stackfilter = __import__('eos-filter-stacktrace')
import pytest

def test_parseThreadStack():
    zmqFilter = stackfilter.ZmqFilter()

    stack = stackfilter.ThreadStack([
      'Thread 76 (Thread 0x7f7f0a6c1700 (LWP 118449)):\n',
      '#0  0x00007f80596242ae in pthread_rwlock_wrlock () from /lib64/libpthread.so.0\n',
      '#1  0x00007f805161ef85 in eos::common::RWMutex::LockWrite (this=0x7f8051ffacd8 <XrdSfsGetFileSystem::myFS+6200>) at ../../common/RWMutex.cc:339\n',
      '#2  0x00007f8051b2075a in XrdMgmOfs::FSctl (this=0x7f8051ff94a0 <XrdSfsGetFileSystem::myFS>, cmd=<optimized out>, args=..., error=..., \n',
      '    client=0x7f7faad8f488) at ../../mgm/XrdMgmOfs/fsctl/Drop.cc:47\n',
      '#3  0x00007f8059d27344 in XrdXrootdProtocol::do_Qopaque (this=0x7f7fc18b2e80, qopt=<optimized out>)\n', '    at /usr/src/debug/xrootd/xrootd/src/XrdXrootd/XrdXrootdXeq.cc:1779\n',
      '#4  0x00007f8059aa1149 in XrdLink::DoIt (this=0x7f7faf3ea2d8) at /usr/src/debug/xrootd/xrootd/src/Xrd/XrdLink.cc:435\n',
      '#5  0x00007f8059aa453f in XrdScheduler::Run (this=0x610e78 <XrdMain::Config+440>) at /usr/src/debug/xrootd/xrootd/src/Xrd/XrdScheduler.cc:357\n',
      '#6  0x00007f8059aa4689 in XrdStartWorking (carg=<optimized out>) at /usr/src/debug/xrootd/xrootd/src/Xrd/XrdScheduler.cc:87\n',
      '#7  0x00007f8059a640f7 in XrdSysThread_Xeq (myargs=0x7f7faf3a1600) at /usr/src/debug/xrootd/xrootd/src/XrdSys/XrdSysPthread.cc:86\n',
      '#8  0x00007f8059620e25 in start_thread () from /lib64/libpthread.so.0\n',
      '#9  0x00007f805892634d in clone () from /lib64/libc.so.6\n',
      '\n'
    ])

    assert stack.getThreadID() == 76
    assert not zmqFilter.check(stack)

    print(stack.getFrame(0))
    assert stack.getFrame(0) == "#0  0x00007f80596242ae in pthread_rwlock_wrlock () from /lib64/libpthread.so.0"
    assert stack.getFrame(1) == "#1  0x00007f805161ef85 in eos::common::RWMutex::LockWrite (this=0x7f8051ffacd8 <XrdSfsGetFileSystem::myFS+6200>) at ../../common/RWMutex.cc:339"
    assert stack.getFrame(2) == "#2  0x00007f8051b2075a in XrdMgmOfs::FSctl (this=0x7f8051ff94a0 <XrdSfsGetFileSystem::myFS>, cmd=<optimized out>, args=..., error=..., client=0x7f7faad8f488) at ../../mgm/XrdMgmOfs/fsctl/Drop.cc:47"
    assert stack.getFrame(3) == "#3  0x00007f8059d27344 in XrdXrootdProtocol::do_Qopaque (this=0x7f7fc18b2e80, qopt=<optimized out>) at /usr/src/debug/xrootd/xrootd/src/XrdXrootd/XrdXrootdXeq.cc:1779"
    assert stack.getFrame(4) == "#4  0x00007f8059aa1149 in XrdLink::DoIt (this=0x7f7faf3ea2d8) at /usr/src/debug/xrootd/xrootd/src/Xrd/XrdLink.cc:435"
    assert stack.getFrame(5) == "#5  0x00007f8059aa453f in XrdScheduler::Run (this=0x610e78 <XrdMain::Config+440>) at /usr/src/debug/xrootd/xrootd/src/Xrd/XrdScheduler.cc:357"
    assert stack.getFrame(6) == "#6  0x00007f8059aa4689 in XrdStartWorking (carg=<optimized out>) at /usr/src/debug/xrootd/xrootd/src/Xrd/XrdScheduler.cc:87"
    assert stack.getFrame(7) == "#7  0x00007f8059a640f7 in XrdSysThread_Xeq (myargs=0x7f7faf3a1600) at /usr/src/debug/xrootd/xrootd/src/XrdSys/XrdSysPthread.cc:86"
    assert stack.getFrame(8) == "#8  0x00007f8059620e25 in start_thread () from /lib64/libpthread.so.0"
    assert stack.getFrame(9) == "#9  0x00007f805892634d in clone () from /lib64/libc.so.6"

    assert stack.getNumberOfFrames() == 10
    assert not zmqFilter.check(stack)

def test_parseThreadStack2():
    zmqFilter = stackfilter.ZmqFilter()
    stack = stackfilter.ThreadStack([
        "Thread 1132 (Thread 0x7fa4d93fc700 (LWP 114706)):\n",
        "#0  0x00007fa5947dd923 in epoll_wait () from /lib64/libc.so.6\n",
        "#1  0x00007fa58ccd4309 in zmq::epoll_t::loop() () from /lib64/libzmq.so.5\n",
        "#2  0x00007fa58cd083a6 in thread_routine () from /lib64/libzmq.so.5\n",
        "#3  0x00007fa5954d7e25 in start_thread () from /lib64/libpthread.so.0\n",
        "#4  0x00007fa5947dd34d in clone () from /lib64/libc.so.6\n"
    ])

    assert stack.getNumberOfFrames() == 5
    assert zmqFilter.check(stack)

    assert stack.tostr() == (
      "Thread 1132 (Thread 0x7fa4d93fc700 (LWP 114706)):\n" +
      "#0  0x00007fa5947dd923 in epoll_wait () from /lib64/libc.so.6\n" +
      "#1  0x00007fa58ccd4309 in zmq::epoll_t::loop() () from /lib64/libzmq.so.5\n" +
      "#2  0x00007fa58cd083a6 in thread_routine () from /lib64/libzmq.so.5\n" +
      "#3  0x00007fa5954d7e25 in start_thread () from /lib64/libpthread.so.0\n" +
      "#4  0x00007fa5947dd34d in clone () from /lib64/libc.so.6"
    )


def test_parseStackTrace():
    zmqFilter = stackfilter.ZmqFilter()

    trace = stackfilter.StackTrace([
      'Thread 99 (Thread 0x7f7f0bdd8700 (LWP 118421)):\n',
      '#0  0x00007f8059624094 in pthread_rwlock_rdlock () from /lib64/libpthread.so.0\n',
      '#1  0x00007f805161e82e in eos::common::RWMutex::LockRead (this=0x7f8051ffacd8 <XrdSfsGetFileSystem::myFS+6200>) at ../../common/RWMutex.cc:258\n',
      '#2  0x00007f8051b30742 in XrdMgmOfs::FSctl (this=0x7f8051ff94a0 <XrdSfsGetFileSystem::myFS>, cmd=<optimized out>, args=..., error=..., \n',
      '    client=<optimized out>) at ../../mgm/XrdMgmOfs/fsctl/Schedule2Delete.cc:67\n',
      '#3  0x00007f8059d27344 in XrdXrootdProtocol::do_Qopaque (this=0x7f7fb2b4f600, qopt=<optimized out>)\n',
      '    at /usr/src/debug/xrootd/xrootd/src/XrdXrootd/XrdXrootdXeq.cc:1779\n',
      '#4  0x00007f8059aa1149 in XrdLink::DoIt (this=0x7f7f8c17dac8) at /usr/src/debug/xrootd/xrootd/src/Xrd/XrdLink.cc:435\n',
      '#5  0x00007f8059aa453f in XrdScheduler::Run (this=0x610e78 <XrdMain::Config+440>) at /usr/src/debug/xrootd/xrootd/src/Xrd/XrdScheduler.cc:357\n',
      '#6  0x00007f8059aa4689 in XrdStartWorking (carg=<optimized out>) at /usr/src/debug/xrootd/xrootd/src/Xrd/XrdScheduler.cc:87\n',
      '#7  0x00007f8059a640f7 in XrdSysThread_Xeq (myargs=0x7f7f83cfc0c0) at /usr/src/debug/xrootd/xrootd/src/XrdSys/XrdSysPthread.cc:86\n',
      '#8  0x00007f8059620e25 in start_thread () from /lib64/libpthread.so.0\n', '#9  0x00007f805892634d in clone () from /lib64/libc.so.6\n',
      '\n',
      'Thread 98 (Thread 0x7f7f0bcd7700 (LWP 118422)):\n',
      '#0  0x00007f80596242ae in pthread_rwlock_wrlock () from /lib64/libpthread.so.0\n',
      '#1  0x00007f805161ef85 in eos::common::RWMutex::LockWrite (this=0x7f8051ffacd8 <XrdSfsGetFileSystem::myFS+6200>) at ../../common/RWMutex.cc:339\n',
      '#2  0x00007f8051b2075a in XrdMgmOfs::FSctl (this=0x7f8051ff94a0 <XrdSfsGetFileSystem::myFS>, cmd=<optimized out>, args=..., error=..., \n',
      '    client=0x7f7fb2b77148) at ../../mgm/XrdMgmOfs/fsctl/Drop.cc:47\n',
      '#3  0x00007f8059d27344 in XrdXrootdProtocol::do_Qopaque (this=0x7f7fb26dd600, qopt=<optimized out>)\n',
      '    at /usr/src/debug/xrootd/xrootd/src/XrdXrootd/XrdXrootdXeq.cc:1779\n',
      '#4  0x00007f8059aa1149 in XrdLink::DoIt (this=0x7f7f8c17dd78) at /usr/src/debug/xrootd/xrootd/src/Xrd/XrdLink.cc:435\n',
      '#5  0x00007f8059aa453f in XrdScheduler::Run (this=0x610e78 <XrdMain::Config+440>) at /usr/src/debug/xrootd/xrootd/src/Xrd/XrdScheduler.cc:357\n',
      '#6  0x00007f8059aa4689 in XrdStartWorking (carg=<optimized out>) at /usr/src/debug/xrootd/xrootd/src/Xrd/XrdScheduler.cc:87\n',
      '#7  0x00007f8059a640f7 in XrdSysThread_Xeq (myargs=0x7f7f8c13f3a0) at /usr/src/debug/xrootd/xrootd/src/XrdSys/XrdSysPthread.cc:86\n',
      '#8  0x00007f8059620e25 in start_thread () from /lib64/libpthread.so.0\n',
      '#9  0x00007f805892634d in clone () from /lib64/libc.so.6\n',
      '\n',
    ])

    assert trace.getNumberOfThreads() == 2
    assert trace.getThread(0).getFrame(4) == "#4  0x00007f8059aa1149 in XrdLink::DoIt (this=0x7f7f8c17dac8) at /usr/src/debug/xrootd/xrootd/src/Xrd/XrdLink.cc:435"

    assert not zmqFilter.check(trace.getThread(0))
    assert not zmqFilter.check(trace.getThread(1))

    assert trace.getThread(0).getFrame(3) != trace.getThread(1).getFrame(3)
    assert trace.getThread(1).getFrame(3) == "#3  0x00007f8059d27344 in XrdXrootdProtocol::do_Qopaque (this=0x7f7fb26dd600, qopt=<optimized out>) at /usr/src/debug/xrootd/xrootd/src/XrdXrootd/XrdXrootdXeq.cc:1779"
