/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "trace.h"

#include <sys/uio.h>

#include "base/unix_file/fd_file.h"
#include "class_linker.h"
#include "common_throws.h"
#include "debugger.h"
#include "dex_file-inl.h"
#include "instrumentation.h"
#include "mirror/abstract_method-inl.h"
#include "mirror/class-inl.h"
#include "mirror/dex_cache.h"
#include "mirror/object_array-inl.h"
#include "mirror/object-inl.h"
#include "object_utils.h"
#include "os.h"
#include "scoped_thread_state_change.h"
#include "thread.h"
#include "thread_list.h"
#if !defined(ART_USE_PORTABLE_COMPILER)
#include "entrypoints/quick/quick_entrypoints.h"
#endif

namespace art {

// File format:
//     header
//     record 0
//     record 1
//     ...
//
// Header format:
//     u4  magic ('SLOW')
//     u2  version
//     u2  offset to data
//     u8  start date/time in usec
//     u2  record size in bytes (version >= 2 only)
//     ... padding to 32 bytes
//
// Record format v1:
//     u1  thread ID
//     u4  method ID | method action
//     u4  time delta since start, in usec
//
// Record format v2:
//     u2  thread ID
//     u4  method ID | method action
//     u4  time delta since start, in usec
//
// Record format v3:
//     u2  thread ID
//     u4  method ID | method action
//     u4  time delta since start, in usec
//     u4  wall time since start, in usec (when clock == "dual" only)
//
// 32 bits of microseconds is 70 minutes.
//
// All values are stored in little-endian order.

enum TraceAction {
    kTraceMethodEnter = 0x00,       // method entry
    kTraceMethodExit = 0x01,        // method exit
    kTraceUnroll = 0x02,            // method exited by exception unrolling
    // 0x03 currently unused
    kTraceMethodActionMask = 0x03,  // two bits
};

static const char     kTraceTokenChar             = '*';
static const uint16_t kTraceHeaderLength          = 32;
static const uint32_t kTraceMagicValue            = 0x574f4c53;
static const uint16_t kTraceVersionSingleClock    = 2;
static const uint16_t kTraceVersionDualClock      = 3;
static const uint16_t kTraceRecordSizeSingleClock = 10;  // using v2
static const uint16_t kTraceRecordSizeDualClock   = 14;  // using v3 with two timestamps

#if defined(HAVE_POSIX_CLOCKS)
ProfilerClockSource Trace::default_clock_source_ = kProfilerClockSourceDual;
#else
ProfilerClockSource Trace::default_clock_source_ = kProfilerClockSourceWall;
#endif

Trace* Trace::the_trace_ = NULL;

static mirror::AbstractMethod* DecodeTraceMethodId(uint32_t tmid) {
  return reinterpret_cast<mirror::AbstractMethod*>(tmid & ~kTraceMethodActionMask);
}

static TraceAction DecodeTraceAction(uint32_t tmid) {
  return static_cast<TraceAction>(tmid & kTraceMethodActionMask);
}

static uint32_t EncodeTraceMethodAndAction(const mirror::AbstractMethod* method,
                                           TraceAction action) {
  uint32_t tmid = reinterpret_cast<uint32_t>(method) | action;
  DCHECK_EQ(method, DecodeTraceMethodId(tmid));
  return tmid;
}

void Trace::SetDefaultClockSource(ProfilerClockSource clock_source) {
#if defined(HAVE_POSIX_CLOCKS)
  default_clock_source_ = clock_source;
#else
  if (clock_source != kProfilerClockSourceWall) {
    LOG(WARNING) << "Ignoring tracing request to use ";
  }
#endif
}

static uint16_t GetTraceVersion(ProfilerClockSource clock_source) {
  return (clock_source == kProfilerClockSourceDual) ? kTraceVersionDualClock
                                                    : kTraceVersionSingleClock;
}

static uint16_t GetRecordSize(ProfilerClockSource clock_source) {
  return (clock_source == kProfilerClockSourceDual) ? kTraceRecordSizeDualClock
                                                    : kTraceRecordSizeSingleClock;
}

bool Trace::UseThreadCpuClock() {
  return (clock_source_ == kProfilerClockSourceThreadCpu) ||
      (clock_source_ == kProfilerClockSourceDual);
}

bool Trace::UseWallClock() {
  return (clock_source_ == kProfilerClockSourceWall) ||
      (clock_source_ == kProfilerClockSourceDual);
}

static void MeasureClockOverhead(Trace* trace) {
  if (trace->UseThreadCpuClock()) {
    ThreadCpuMicroTime();
  }
  if (trace->UseWallClock()) {
    MicroTime();
  }
}

static uint32_t GetClockOverhead(Trace* trace) {
  uint64_t start = ThreadCpuMicroTime();

  for (int i = 4000; i > 0; i--) {
    MeasureClockOverhead(trace);
    MeasureClockOverhead(trace);
    MeasureClockOverhead(trace);
    MeasureClockOverhead(trace);
    MeasureClockOverhead(trace);
    MeasureClockOverhead(trace);
    MeasureClockOverhead(trace);
    MeasureClockOverhead(trace);
  }

  uint64_t elapsed = ThreadCpuMicroTime() - start;
  return uint32_t (elapsed / 32);
}

// TODO: put this somewhere with the big-endian equivalent used by JDWP.
static void Append2LE(uint8_t* buf, uint16_t val) {
  *buf++ = (uint8_t) val;
  *buf++ = (uint8_t) (val >> 8);
}

// TODO: put this somewhere with the big-endian equivalent used by JDWP.
static void Append4LE(uint8_t* buf, uint32_t val) {
  *buf++ = (uint8_t) val;
  *buf++ = (uint8_t) (val >> 8);
  *buf++ = (uint8_t) (val >> 16);
  *buf++ = (uint8_t) (val >> 24);
}

// TODO: put this somewhere with the big-endian equivalent used by JDWP.
static void Append8LE(uint8_t* buf, uint64_t val) {
  *buf++ = (uint8_t) val;
  *buf++ = (uint8_t) (val >> 8);
  *buf++ = (uint8_t) (val >> 16);
  *buf++ = (uint8_t) (val >> 24);
  *buf++ = (uint8_t) (val >> 32);
  *buf++ = (uint8_t) (val >> 40);
  *buf++ = (uint8_t) (val >> 48);
  *buf++ = (uint8_t) (val >> 56);
}

void Trace::Start(const char* trace_filename, int trace_fd, int buffer_size, int flags,
                  bool direct_to_ddms) {
  Thread* self = Thread::Current();
  {
    MutexLock mu(self, *Locks::trace_lock_);
    if (the_trace_ != NULL) {
      LOG(ERROR) << "Trace already in progress, ignoring this request";
      return;
    }
  }
  Runtime* runtime = Runtime::Current();
  runtime->GetThreadList()->SuspendAll();

  // Open trace file if not going directly to ddms.
  UniquePtr<File> trace_file;
  if (!direct_to_ddms) {
    if (trace_fd < 0) {
      trace_file.reset(OS::OpenFile(trace_filename, true));
    } else {
      trace_file.reset(new File(trace_fd, "tracefile"));
      trace_file->DisableAutoClose();
    }
    if (trace_file.get() == NULL) {
      PLOG(ERROR) << "Unable to open trace file '" << trace_filename << "'";
      runtime->GetThreadList()->ResumeAll();
      ScopedObjectAccess soa(self);
      ThrowRuntimeException("Unable to open trace file '%s'", trace_filename);
      return;
    }
  }

  // Create Trace object.
  {
    MutexLock mu(self, *Locks::trace_lock_);
    if (the_trace_ != NULL) {
      LOG(ERROR) << "Trace already in progress, ignoring this request";
    } else {
      the_trace_ = new Trace(trace_file.release(), buffer_size, flags);

      // Enable count of allocs if specified in the flags.
      if ((flags && kTraceCountAllocs) != 0) {
        runtime->SetStatsEnabled(true);
      }

      runtime->GetInstrumentation()->AddListener(the_trace_,
                                                 instrumentation::Instrumentation::kMethodEntered |
                                                 instrumentation::Instrumentation::kMethodExited |
                                                 instrumentation::Instrumentation::kMethodUnwind);
    }
  }
  runtime->GetThreadList()->ResumeAll();
}

void Trace::Stop() {
  Runtime* runtime = Runtime::Current();
  runtime->GetThreadList()->SuspendAll();
  Trace* the_trace = NULL;
  {
    MutexLock mu(Thread::Current(), *Locks::trace_lock_);
    if (the_trace_ == NULL) {
      LOG(ERROR) << "Trace stop requested, but no trace currently running";
    } else {
      the_trace = the_trace_;
      the_trace_ = NULL;
    }
  }
  if (the_trace != NULL) {
    the_trace->FinishTracing();
    runtime->GetInstrumentation()->RemoveListener(the_trace,
                                                  instrumentation::Instrumentation::kMethodEntered |
                                                  instrumentation::Instrumentation::kMethodExited |
                                                  instrumentation::Instrumentation::kMethodUnwind);
    delete the_trace;
  }
  runtime->GetThreadList()->ResumeAll();
}

void Trace::Shutdown() {
  if (IsMethodTracingActive()) {
    Stop();
  }
}

bool Trace::IsMethodTracingActive() {
  MutexLock mu(Thread::Current(), *Locks::trace_lock_);
  return the_trace_ != NULL;
}

Trace::Trace(File* trace_file, int buffer_size, int flags)
    : trace_file_(trace_file), buf_(new uint8_t[buffer_size]()), flags_(flags),
      clock_source_(default_clock_source_), buffer_size_(buffer_size), start_time_(MicroTime()),
      cur_offset_(0),  overflow_(false) {
  // Set up the beginning of the trace.
  uint16_t trace_version = GetTraceVersion(clock_source_);
  memset(buf_.get(), 0, kTraceHeaderLength);
  Append4LE(buf_.get(), kTraceMagicValue);
  Append2LE(buf_.get() + 4, trace_version);
  Append2LE(buf_.get() + 6, kTraceHeaderLength);
  Append8LE(buf_.get() + 8, start_time_);
  if (trace_version >= kTraceVersionDualClock) {
    uint16_t record_size = GetRecordSize(clock_source_);
    Append2LE(buf_.get() + 16, record_size);
  }

  // Update current offset.
  cur_offset_ = kTraceHeaderLength;
}

static void DumpBuf(uint8_t* buf, size_t buf_size, ProfilerClockSource clock_source)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  uint8_t* ptr = buf + kTraceHeaderLength;
  uint8_t* end = buf + buf_size;

  while (ptr < end) {
    uint32_t tmid = ptr[2] | (ptr[3] << 8) | (ptr[4] << 16) | (ptr[5] << 24);
    mirror::AbstractMethod* method = DecodeTraceMethodId(tmid);
    TraceAction action = DecodeTraceAction(tmid);
    LOG(INFO) << PrettyMethod(method) << " " << static_cast<int>(action);
    ptr += GetRecordSize(clock_source);
  }
}

void Trace::FinishTracing() {
  // Compute elapsed time.
  uint64_t elapsed = MicroTime() - start_time_;

  size_t final_offset = cur_offset_;
  uint32_t clock_overhead = GetClockOverhead(this);

  if ((flags_ & kTraceCountAllocs) != 0) {
    Runtime::Current()->SetStatsEnabled(false);
  }

  std::set<mirror::AbstractMethod*> visited_methods;
  GetVisitedMethods(final_offset, &visited_methods);

  std::ostringstream os;

  os << StringPrintf("%cversion\n", kTraceTokenChar);
  os << StringPrintf("%d\n", GetTraceVersion(clock_source_));
  os << StringPrintf("data-file-overflow=%s\n", overflow_ ? "true" : "false");
  if (UseThreadCpuClock()) {
    if (UseWallClock()) {
      os << StringPrintf("clock=dual\n");
    } else {
      os << StringPrintf("clock=thread-cpu\n");
    }
  } else {
    os << StringPrintf("clock=wall\n");
  }
  os << StringPrintf("elapsed-time-usec=%llu\n", elapsed);
  size_t num_records = (final_offset - kTraceHeaderLength) / GetRecordSize(clock_source_);
  os << StringPrintf("num-method-calls=%zd\n", num_records);
  os << StringPrintf("clock-call-overhead-nsec=%d\n", clock_overhead);
  os << StringPrintf("vm=art\n");
  if ((flags_ & kTraceCountAllocs) != 0) {
    os << StringPrintf("alloc-count=%d\n", Runtime::Current()->GetStat(KIND_ALLOCATED_OBJECTS));
    os << StringPrintf("alloc-size=%d\n", Runtime::Current()->GetStat(KIND_ALLOCATED_BYTES));
    os << StringPrintf("gc-count=%d\n", Runtime::Current()->GetStat(KIND_GC_INVOCATIONS));
  }
  os << StringPrintf("%cthreads\n", kTraceTokenChar);
  DumpThreadList(os);
  os << StringPrintf("%cmethods\n", kTraceTokenChar);
  DumpMethodList(os, visited_methods);
  os << StringPrintf("%cend\n", kTraceTokenChar);

  std::string header(os.str());
  if (trace_file_.get() == NULL) {
    iovec iov[2];
    iov[0].iov_base = reinterpret_cast<void*>(const_cast<char*>(header.c_str()));
    iov[0].iov_len = header.length();
    iov[1].iov_base = buf_.get();
    iov[1].iov_len = final_offset;
    Dbg::DdmSendChunkV(CHUNK_TYPE("MPSE"), iov, 2);
    const bool kDumpTraceInfo = false;
    if (kDumpTraceInfo) {
      LOG(INFO) << "Trace sent:\n" << header;
      DumpBuf(buf_.get(), final_offset, clock_source_);
    }
  } else {
    if (!trace_file_->WriteFully(header.c_str(), header.length()) ||
        !trace_file_->WriteFully(buf_.get(), final_offset)) {
      std::string detail(StringPrintf("Trace data write failed: %s", strerror(errno)));
      PLOG(ERROR) << detail;
      ThrowRuntimeException("%s", detail.c_str());
    }
  }
}

void Trace::DexPcMoved(Thread* thread, mirror::Object* this_object,
                       const mirror::AbstractMethod* method, uint32_t new_dex_pc) {
  // We're not recorded to listen to this kind of event, so complain.
  LOG(ERROR) << "Unexpected dex PC event in tracing " << PrettyMethod(method) << " " << new_dex_pc;
};

void Trace::MethodEntered(Thread* thread, mirror::Object* this_object,
                          const mirror::AbstractMethod* method, uint32_t dex_pc) {
  LogMethodTraceEvent(thread, method, instrumentation::Instrumentation::kMethodEntered);
}

void Trace::MethodExited(Thread* thread, mirror::Object* this_object,
                         const mirror::AbstractMethod* method, uint32_t dex_pc,
                         const JValue& return_value) {
  UNUSED(return_value);
  LogMethodTraceEvent(thread, method, instrumentation::Instrumentation::kMethodExited);
}

void Trace::MethodUnwind(Thread* thread, const mirror::AbstractMethod* method, uint32_t dex_pc) {
  LogMethodTraceEvent(thread, method, instrumentation::Instrumentation::kMethodUnwind);
}

void Trace::ExceptionCaught(Thread* thread, const ThrowLocation& throw_location,
                            mirror::AbstractMethod* catch_method, uint32_t catch_dex_pc,
                            mirror::Throwable* exception_object)
    SHARED_LOCKS_REQUIRED(Locks::mutator_lock_) {
  LOG(ERROR) << "Unexpected exception caught event in tracing";
}

void Trace::LogMethodTraceEvent(Thread* thread, const mirror::AbstractMethod* method,
                                instrumentation::Instrumentation::InstrumentationEvent event) {
  // Advance cur_offset_ atomically.
  int32_t new_offset;
  int32_t old_offset;
  do {
    old_offset = cur_offset_;
    new_offset = old_offset + GetRecordSize(clock_source_);
    if (new_offset > buffer_size_) {
      overflow_ = true;
      return;
    }
  } while (android_atomic_release_cas(old_offset, new_offset, &cur_offset_) != 0);

  TraceAction action = kTraceMethodEnter;
  switch (event) {
    case instrumentation::Instrumentation::kMethodEntered:
      action = kTraceMethodEnter;
      break;
    case instrumentation::Instrumentation::kMethodExited:
      action = kTraceMethodExit;
      break;
    case instrumentation::Instrumentation::kMethodUnwind:
      action = kTraceUnroll;
      break;
    default:
      UNIMPLEMENTED(FATAL) << "Unexpected event: " << event;
  }

  uint32_t method_value = EncodeTraceMethodAndAction(method, action);

  // Write data
  uint8_t* ptr = buf_.get() + old_offset;
  Append2LE(ptr, thread->GetTid());
  Append4LE(ptr + 2, method_value);
  ptr += 6;

  if (UseThreadCpuClock()) {
    // TODO: this isn't vaguely thread safe.
    SafeMap<Thread*, uint64_t>::iterator it = thread_clock_base_map_.find(thread);
    uint32_t thread_clock_diff = 0;
    if (UNLIKELY(it == thread_clock_base_map_.end())) {
      // First event, the diff is 0, record the base time in the map.
      uint64_t time = ThreadCpuMicroTime();
      thread_clock_base_map_.Put(thread, time);
    } else {
      uint64_t thread_clock_base = it->second;
      thread_clock_diff = ThreadCpuMicroTime() - thread_clock_base;
    }
    Append4LE(ptr, thread_clock_diff);
    ptr += 4;
  }
  if (UseWallClock()) {
    uint32_t wall_clock_diff = MicroTime() - start_time_;
    Append4LE(ptr, wall_clock_diff);
  }
}

void Trace::GetVisitedMethods(size_t buf_size,
                              std::set<mirror::AbstractMethod*>* visited_methods) {
  uint8_t* ptr = buf_.get() + kTraceHeaderLength;
  uint8_t* end = buf_.get() + buf_size;

  while (ptr < end) {
    uint32_t tmid = ptr[2] | (ptr[3] << 8) | (ptr[4] << 16) | (ptr[5] << 24);
    mirror::AbstractMethod* method = DecodeTraceMethodId(tmid);
    visited_methods->insert(method);
    ptr += GetRecordSize(clock_source_);
  }
}

void Trace::DumpMethodList(std::ostream& os,
                           const std::set<mirror::AbstractMethod*>& visited_methods) {
  typedef std::set<mirror::AbstractMethod*>::const_iterator It;  // TODO: C++0x auto
  MethodHelper mh;
  for (It it = visited_methods.begin(); it != visited_methods.end(); ++it) {
    mirror::AbstractMethod* method = *it;
    mh.ChangeMethod(method);
    os << StringPrintf("%p\t%s\t%s\t%s\t%s\n", method,
        PrettyDescriptor(mh.GetDeclaringClassDescriptor()).c_str(), mh.GetName(),
        mh.GetSignature().c_str(), mh.GetDeclaringClassSourceFile());
  }
}

static void DumpThread(Thread* t, void* arg) {
  std::ostream& os = *reinterpret_cast<std::ostream*>(arg);
  std::string name;
  t->GetThreadName(name);
  os << t->GetTid() << "\t" << name << "\n";
}

void Trace::DumpThreadList(std::ostream& os) {
  Thread* self = Thread::Current();
  Locks::thread_list_lock_->AssertNotHeld(self);
  MutexLock mu(self, *Locks::thread_list_lock_);
  Runtime::Current()->GetThreadList()->ForEach(DumpThread, &os);
}

}  // namespace art