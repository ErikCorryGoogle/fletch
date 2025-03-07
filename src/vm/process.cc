// Copyright (c) 2014, the Fletch project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE.md file.

#include "src/vm/process.h"

#include <errno.h>
#include <stdlib.h>

#include "src/shared/assert.h"
#include "src/shared/bytecodes.h"
#include "src/shared/flags.h"
#include "src/shared/names.h"
#include "src/shared/selectors.h"

#include "src/vm/frame.h"
#include "src/vm/heap_validator.h"
#include "src/vm/mark_sweep.h"
#include "src/vm/natives.h"
#include "src/vm/object_memory.h"
#include "src/vm/port.h"
#include "src/vm/process_queue.h"
#include "src/vm/session.h"
#include "src/vm/storebuffer.h"

namespace fletch {

static uword kPreemptMarker = 1 << 0;
static uword kProfileMarker = 1 << 1;
static uword kDebugInterruptMarker = 1 << 2;
static uword kMaxStackMarker = ~static_cast<uword>((1 << 3) - 1);

ThreadState::ThreadState()
    : thread_id_(-1),
      queue_(new ProcessQueue()),
      cache_(NULL),
      idle_monitor_(Platform::CreateMonitor()),
      next_idle_thread_(NULL) {}

void ThreadState::AttachToCurrentThread() { thread_ = ThreadIdentifier(); }

LookupCache* ThreadState::EnsureCache() {
  if (cache_ == NULL) cache_ = new LookupCache();
  return cache_;
}

ThreadState::~ThreadState() {
  delete idle_monitor_;
  delete queue_;
  delete cache_;
}

Process::Process(Program* program, Process* parent)
    : coroutine_(NULL),
      stack_limit_(0),
      program_(program),
      statics_(NULL),
      exception_(program->null_object()),
      primary_lookup_cache_(NULL),
      random_(program->random()->NextUInt32() + 1),
#ifdef FLETCH_ENABLE_MULTIPLE_PROCESS_HEAPS
      heap_(&random_, 4 * KB),
#endif
      immutable_heap_(NULL),
      state_(kSleeping),
      thread_state_(NULL),
      next_(NULL),
      queue_(NULL),
      queue_next_(NULL),
      queue_previous_(NULL),
      signal_(NULL),
      process_handle_(NULL),
      ports_(NULL),
      process_list_next_(NULL),
      process_list_prev_(NULL),
      process_triangle_count_(1),
      parent_(parent),
      errno_cache_(0),
      debug_info_(NULL) {
  process_handle_ = new ProcessHandle(this);

  // These asserts need to hold when running on the target, but they don't need
  // to hold on the host (the build machine, where the interpreter-generating
  // program runs).  We put these asserts here on the assumption that the
  // interpreter-generating program will not instantiate this class.
  static_assert(kCoroutineOffset == offsetof(Process, coroutine_),
                "coroutine_");
  static_assert(kStackLimitOffset == offsetof(Process, stack_limit_),
                "stack_limit_");
  static_assert(kProgramOffset == offsetof(Process, program_), "program_");
  static_assert(kStaticsOffset == offsetof(Process, statics_), "statics_");
  static_assert(kExceptionOffset == offsetof(Process, exception_),
                "exception_");
  static_assert(
      kPrimaryLookupCacheOffset == offsetof(Process, primary_lookup_cache_),
      "primary_lookup_cache_");

  Array* static_fields = program->static_fields();
  int length = static_fields->length();
  statics_ = Array::cast(NewArray(length));
  for (int i = 0; i < length; i++) {
    statics_->set(i, static_fields->get(i));
  }
#ifdef DEBUG
  true_then_false_ = true;
#endif
}

Process::~Process() {
  // [Cleanup] should've been called at this point. So we ASSERT the post
  // conditions here.
  ASSERT(ports_ == NULL);

  links()->NotifyMonitors(process_handle());

  ProcessHandle::DecrementRef(process_handle_);

  Signal* signal = signal_.load();
  if (signal != NULL) Signal::DecrementRef(signal);

#ifdef FLETCH_ENABLE_MULTIPLE_PROCESS_HEAPS
  heap_.ProcessWeakPointers();
#endif  // #ifdef FLETCH_ENABLE_MULTIPLE_PROCESS_HEAPS

  delete debug_info_;

  ASSERT(next_ == NULL);
  ASSERT(cooked_stack_deltas_.is_empty());
}

void Process::Cleanup(Signal::Kind kind) {
  EventHandler* event_handler = program()->event_handler();
  event_handler->ReceiverForPortsDied(ports_);

  // Clear out the process pointer from all the ports.
  ASSERT(immutable_heap_ == NULL);
  while (ports_ != NULL) {
    Port* next = ports_->next();
    ports_->OwnerProcessTerminating();
    ports_ = next;
  }

  // We are going down at this point. If anything else is starting to
  // link/monitor with this [ProcessHandle], it will fail after this line.
  process_handle_->OwnerProcessTerminating();

  // Since nobody can send us messages (or signals) at this point, we send a
  // signal to all linked processes.
  links()->NotifyLinkedProcesses(process_handle(), kind);
}

void Process::SetupExecutionStack() {
  ASSERT(coroutine_ == NULL);
  Stack* stack = Stack::cast(NewStack(256));
  stack->set(0, NULL);
  Coroutine* coroutine =
      Coroutine::cast(NewInstance(program()->coroutine_class()));
  coroutine->set_stack(stack);
  UpdateCoroutine(coroutine);
}

void Process::UpdateCoroutine(Coroutine* coroutine) {
  ASSERT(coroutine->has_stack());
  coroutine_ = coroutine;
  UpdateStackLimit();
  store_buffer_.Insert(coroutine->stack());
}

Process::StackCheckResult Process::HandleStackOverflow(int addition) {
  uword current_limit = stack_limit();

  if (current_limit >= kMaxStackMarker) {
    if ((current_limit & kPreemptMarker) != 0) {
      ClearStackMarker(kPreemptMarker);
      UpdateStackLimit();
      return kStackCheckInterrupt;
    }

    if ((current_limit & kDebugInterruptMarker) != 0) {
      ClearStackMarker(kDebugInterruptMarker);
      UpdateStackLimit();
      return kStackCheckDebugInterrupt;
    }

    if ((current_limit & kProfileMarker) != 0) {
      ClearStackMarker(kProfileMarker);
      UpdateStackLimit();
      return kStackCheckContinue;
    }
  }

  int size_increase = Utils::RoundUpToPowerOfTwo(addition);
  size_increase = Utils::Maximum(256, size_increase);
  int new_size = stack()->length() + size_increase;
  if (new_size > Platform::MaxStackSizeInWords()) return kStackCheckOverflow;

  Object* new_stack_object = NewStack(new_size);
  if (new_stack_object == Failure::retry_after_gc()) {
    CollectMutableGarbage();
    new_stack_object = NewStack(new_size);
    if (new_stack_object == Failure::retry_after_gc()) {
      return kStackCheckOverflow;
    }
  }

  Stack* new_stack = Stack::cast(new_stack_object);
  word height = stack()->length() - stack()->top();
  ASSERT(height >= 0);
  new_stack->set_top(new_stack->length() - height);
  memcpy(new_stack->Pointer(new_stack->top()), stack()->Pointer(stack()->top()),
         height * kWordSize);
  new_stack->UpdateFramePointers(stack());
  ASSERT(coroutine_->has_stack());
  coroutine_->set_stack(new_stack);
  store_buffer_.Insert(coroutine_->stack());
  UpdateStackLimit();
  return kStackCheckContinue;
}

Object* Process::NewByteArray(int length) {
  Class* byte_array_class = program()->byte_array_class();
  return immutable_heap_->CreateByteArray(byte_array_class, length);
}

Object* Process::NewArray(int length) {
  Class* array_class = program()->array_class();
  Object* null = program()->null_object();
  Object* result = heap()->CreateArray(array_class, length, null);
  return result;
}

Object* Process::NewDouble(fletch_double value) {
  Class* double_class = program()->double_class();
  Object* result = immutable_heap_->CreateDouble(double_class, value);
  return result;
}

Object* Process::NewInteger(int64 value) {
  Class* large_integer_class = program()->large_integer_class();
  Object* result =
      immutable_heap_->CreateLargeInteger(large_integer_class, value);
  return result;
}

void Process::TryDeallocInteger(LargeInteger* object) {
  immutable_heap_->TryDeallocInteger(object);
}

Object* Process::NewOneByteString(int length) {
  Class* string_class = program()->one_byte_string_class();
  Object* raw_result =
      immutable_heap_->CreateOneByteString(string_class, length);
  if (raw_result->IsFailure()) return raw_result;
  return OneByteString::cast(raw_result);
}

Object* Process::NewTwoByteString(int length) {
  Class* string_class = program()->two_byte_string_class();
  Object* raw_result =
      immutable_heap_->CreateTwoByteString(string_class, length);
  if (raw_result->IsFailure()) return raw_result;
  return TwoByteString::cast(raw_result);
}

Object* Process::NewOneByteStringUninitialized(int length) {
  Class* string_class = program()->one_byte_string_class();
  Object* raw_result =
      immutable_heap_->CreateOneByteStringUninitialized(string_class, length);
  if (raw_result->IsFailure()) return raw_result;
  return OneByteString::cast(raw_result);
}

Object* Process::NewTwoByteStringUninitialized(int length) {
  Class* string_class = program()->two_byte_string_class();
  Object* raw_result =
      immutable_heap_->CreateTwoByteStringUninitialized(string_class, length);
  if (raw_result->IsFailure()) return raw_result;
  return TwoByteString::cast(raw_result);
}

Object* Process::NewStringFromAscii(List<const char> value) {
  Class* string_class = program()->one_byte_string_class();
  Object* raw_result = immutable_heap_->CreateOneByteStringUninitialized(
      string_class, value.length());
  if (raw_result->IsFailure()) return raw_result;
  OneByteString* result = OneByteString::cast(raw_result);
  for (int i = 0; i < value.length(); i++) {
    result->set_char_code(i, value[i]);
  }
  return result;
}

Object* Process::NewBoxed(Object* value) {
  Class* boxed_class = program()->boxed_class();
  Object* result = heap()->CreateBoxed(boxed_class, value);
  if (result->IsFailure()) return result;
  return result;
}

Object* Process::NewInstance(Class* klass, bool immutable) {
  Object* null = program()->null_object();
  if (immutable) {
    return immutable_heap_->CreateInstance(klass, null, immutable);
  } else {
    return heap()->CreateInstance(klass, null, immutable);
  }
}

Object* Process::ToInteger(int64 value) {
  return Smi::IsValid(value) ? Smi::FromWord(value) : NewInteger(value);
}

Object* Process::NewStack(int length) {
  Class* stack_class = program()->stack_class();
  Object* result = heap()->CreateStack(stack_class, length);

  if (result->IsFailure()) return result;
  store_buffer_.Insert(HeapObject::cast(result));
  return result;
}

struct HeapUsage {
  uint64 timestamp = 0;
  uword process_used = 0;
  uword process_size = 0;
  uword immutable_used = 0;
  uword immutable_size = 0;
  uword program_used = 0;
  uword program_size = 0;

  uword TotalUsed() { return process_used + immutable_used + program_used; }
  uword TotalSize() { return process_used + immutable_size + program_size; }
};

#ifdef FLETCH_ENABLE_MULTIPLE_PROCESS_HEAPS

static void GetHeapUsage(Process* process, HeapUsage* heap_usage) {
  heap_usage->timestamp = Platform::GetMicroseconds();
  heap_usage->process_used = process->heap()->space()->Used();
  heap_usage->process_size = process->heap()->space()->Size();
  heap_usage->immutable_used =
      process->program()->shared_heap()->EstimatedUsed();
  heap_usage->immutable_size =
      process->program()->shared_heap()->EstimatedSize();
  heap_usage->program_used = process->program()->heap()->space()->Used();
  heap_usage->program_size = process->program()->heap()->space()->Size();
}

void PrintProcessGCInfo(Process* process, HeapUsage* before, HeapUsage* after) {
  static int count = 0;
  if ((count & 0xF) == 0) {
    Print::Error(
        "Program-GC-Info, \tElapsed, \tProcess use/size, \tImmutable use/size,"
        " \tProgram use/size, \tTotal heap\n");
  }
  Print::Error(
      "Process-GC(%i, %p): "
      "\t%lli us, "
      "\t%lu/%lu -> %lu/%lu, "
      "\t%lu/%lu, "
      "\t%lu/%lu, "
      "\t%lu/%lu -> %lu/%lu\n",
      count++, process, after->timestamp - before->timestamp,
      before->process_used, before->process_size, after->process_used,
      after->process_size, after->immutable_used, after->immutable_size,
      after->program_used, after->program_size, before->TotalUsed(),
      before->TotalSize(), after->TotalUsed(), after->TotalSize());
}

#endif  // #ifdef FLETCH_ENABLE_MULTIPLE_PROCESS_HEAPS

#ifdef FLETCH_ENABLE_MULTIPLE_PROCESS_HEAPS

void Process::CollectMutableGarbage() {
  TakeChildHeaps();

  HeapUsage usage_before;
  if (Flags::print_heap_statistics) {
    GetHeapUsage(this, &usage_before);
  }

  Space* from = heap()->space();
  Space* to = new Space(from->Used() / 10);
  StoreBuffer sb;

  // While garbage collecting, do not fail allocations. Instead grow
  // the to-space as needed.
  NoAllocationFailureScope scope(to);

  ScavengeVisitor visitor(from, to);
  IterateRoots(&visitor);

  ASSERT(!to->is_empty());
  Space* program_space = program()->heap()->space();
  to->CompleteScavengeMutable(&visitor, program_space, &sb);
  store_buffer_.ReplaceAfterMutableGC(&sb);

  heap()->ProcessWeakPointers();
  set_ports(Port::CleanupPorts(from, ports()));
  heap()->ReplaceSpace(to);

  if (Flags::print_heap_statistics) {
    HeapUsage usage_after;
    GetHeapUsage(this, &usage_after);
    PrintProcessGCInfo(this, &usage_before, &usage_after);
  }

  UpdateStackLimit();
}

#else  // #ifdef FLETCH_ENABLE_MULTIPLE_PROCESS_HEAPS

void Process::CollectMutableGarbage() {
  program()->CollectSharedGarbage(true);
  UpdateStackLimit();
}

#endif  // #ifdef FLETCH_ENABLE_MULTIPLE_PROCESS_HEAPS

// Helper class for copying HeapObjects and chaining stacks for a
// process.
class ScavengeAndChainStacksVisitor : public PointerVisitor {
 public:
  ScavengeAndChainStacksVisitor(Process* process, Space* from, Space* to)
      : process_(process), from_(from), to_(to), number_of_stacks_(0) {}

  void VisitBlock(Object** start, Object** end) {
    // Copy all HeapObject pointers in [start, end)
    for (Object** p = start; p < end; p++) ScavengePointerAndChainStack(p);
  }

  int number_of_stacks() const { return number_of_stacks_; }

 private:
  void ChainStack(Stack* stack) {
    number_of_stacks_++;
    Stack* process_stack = process_->stack();
    if (process_stack != stack) {
      // We rely on the fact that the current coroutine stack is
      // visited first.
      ASSERT(to_->Includes(reinterpret_cast<uword>(process_stack)));
      stack->set_next(process_stack->next());
      process_stack->set_next(stack);
    }
  }

  void ScavengePointerAndChainStack(Object** p) {
    Object* object = *p;
    if (!object->IsHeapObject()) return;
    if (!from_->Includes(reinterpret_cast<uword>(object))) return;
    bool forwarded = HeapObject::cast(object)->forwarding_address() != NULL;
    *p = reinterpret_cast<HeapObject*>(object)->CloneInToSpace(to_);
    if (!forwarded) {
      if ((*p)->IsStack()) {
        ChainStack(Stack::cast(*p));
      }
    }
  }

  Process* process_;
  Space* from_;
  Space* to_;
  int number_of_stacks_;
};

#ifdef FLETCH_MARK_SWEEP

int Process::CollectMutableGarbageAndChainStacks() {
  // Mark all reachable objects.
  Space* space = heap()->space();
  MarkingStack stack;
  MarkAndChainStacksVisitor marking_visitor(this, space, &stack);

  // Visit the current coroutine stack first and chain the rest of the
  // stacks starting from there.
  marking_visitor.Visit(coroutine_->stack_address());
  IterateRoots(&marking_visitor);
  stack.Process(&marking_visitor);

  // Weak processing.
  heap()->ProcessWeakPointers();
  set_ports(Port::CleanupPorts(space, ports()));

  // Flush outstanding free_list chunks into the free list. Then sweep
  // over the heap and rebuild the freelist.
  space->Flush();
  SweepingVisitor sweeping_visitor(space->free_list());
  space->IterateObjects(&sweeping_visitor);

  UpdateStackLimit();
  return marking_visitor.number_of_stacks();
}

#else  // #ifdef FLETCH_MARK_SWEEP

int Process::CollectMutableGarbageAndChainStacks() {
  Space* from = heap()->space();
  Space* to = new Space(from->Used() / 10);
  StoreBuffer sb;

  // While garbage collecting, do not fail allocations. Instead grow
  // the to-space as needed.
  NoAllocationFailureScope scope(to);
  ScavengeAndChainStacksVisitor visitor(this, from, to);

  // Visit the current coroutine stack first and chain the rest of the
  // stacks starting from there.
  visitor.Visit(reinterpret_cast<Object**>(coroutine_->stack_address()));
  IterateRoots(&visitor);
  Space* program_space = program()->heap()->space();
  to->CompleteScavengeMutable(&visitor, program_space, &sb);
  store_buffer_.ReplaceAfterMutableGC(&sb);

  heap()->ProcessWeakPointers();
  set_ports(Port::CleanupPorts(from, ports()));
  heap()->ReplaceSpace(to);
  UpdateStackLimit();
  return visitor.number_of_stacks();
}

#endif  // #ifdef FLETCH_MARK_SWEEP

int Process::CollectGarbageAndChainStacks() {
  // NOTE: We need to take all spaces which are getting merged into our
  // heap, because otherwise we'll not update the pointers it has to the
  // program space / to the process heap.
  TakeChildHeaps();

  int number_of_stacks = CollectMutableGarbageAndChainStacks();
  return number_of_stacks;
}

void Process::ValidateHeaps(SharedHeap* shared_heap) {
  ProcessHeapValidatorVisitor v(program()->heap(), shared_heap);
  v.VisitProcess(this);
}

void Process::IterateRoots(PointerVisitor* visitor) {
  visitor->Visit(reinterpret_cast<Object**>(&statics_));
  visitor->Visit(reinterpret_cast<Object**>(&coroutine_));
  visitor->Visit(reinterpret_cast<Object**>(&exception_));
  if (debug_info_ != NULL) debug_info_->VisitPointers(visitor);

  mailbox_.IteratePointers(visitor);
}

void Process::IterateProgramPointers(PointerVisitor* visitor) {
  ASSERT(stacks_are_cooked());
  HeapObjectPointerVisitor program_pointer_visitor(visitor);
  heap()->IterateObjects(&program_pointer_visitor);
  store_buffer_.IteratePointersToImmutableSpace(visitor);
  if (debug_info_ != NULL) debug_info_->VisitProgramPointers(visitor);
  visitor->Visit(&exception_);
  mailbox_.IteratePointers(visitor);
}

void Process::TakeLookupCache() {
  ASSERT(primary_lookup_cache_ == NULL);
  if (program()->is_compact()) return;
  ThreadState* state = thread_state_;
  ASSERT(state != NULL);
  LookupCache* cache = state->EnsureCache();
  primary_lookup_cache_ = cache->primary();
}

void Process::SetStackMarker(uword marker) {
  uword stack_limit = stack_limit_;
  while (true) {
    uword updated_limit =
        stack_limit < kMaxStackMarker ? kMaxStackMarker : stack_limit;
    updated_limit |= marker;
    if (stack_limit_.compare_exchange_weak(stack_limit, updated_limit)) break;
  }
}

void Process::ClearStackMarker(uword marker) {
  uword stack_limit = stack_limit_;
  while (true) {
    ASSERT((stack_limit & marker) != 0);
    uword updated_limit = stack_limit & (~marker);
    if (stack_limit_.compare_exchange_weak(stack_limit, updated_limit)) break;
  }
}

void Process::Preempt() { SetStackMarker(kPreemptMarker); }

void Process::DebugInterrupt() { SetStackMarker(kDebugInterruptMarker); }

void Process::Profile() { SetStackMarker(kProfileMarker); }

void Process::EnsureDebuggerAttached() {
  if (debug_info_ == NULL) debug_info_ = new DebugInfo();
}

int Process::PrepareStepOver() {
  EnsureDebuggerAttached();

  Frame frame(stack());
  frame.MovePrevious();

  uint8_t* current_bcp = frame.ByteCodePointer();
  Opcode opcode = static_cast<Opcode>(*current_bcp);
  if (!Bytecode::IsInvokeVariant(opcode)) {
    // For non-invoke bytecodes step over is the same as step.
    debug_info_->SetStepping();
    return DebugInfo::kNoBreakpointId;
  }

  // TODO(ager): We should consider making this less bytecode-specific.
  int stack_diff = 0;
  switch (opcode) {
    // For invoke bytecodes we set a one-shot breakpoint for the next bytecode
    // with the expected stack height on return.
    case Opcode::kInvokeMethodUnfold:
    case Opcode::kInvokeNoSuchMethod:
    case Opcode::kInvokeMethod: {
      int selector = Utils::ReadInt32(current_bcp + 1);
      int arity = Selector::ArityField::decode(selector);
      stack_diff = -arity;
      break;
    }
    case Opcode::kInvokeStatic:
    case Opcode::kInvokeFactory: {
      int method = Utils::ReadInt32(current_bcp + 1);
      Function* function = program()->static_method_at(method);
      stack_diff = 1 - function->arity();
      break;
    }
    case Opcode::kInvokeStaticUnfold:
    case Opcode::kInvokeFactoryUnfold: {
      Function* function =
          Function::cast(Function::ConstantForBytecode(current_bcp));
      stack_diff = 1 - function->arity();
      break;
    }
    default:
      stack_diff = Bytecode::StackDiff(opcode);
      break;
  }

  Function* function = Function::FromBytecodePointer(current_bcp);
  word frame_end = stack()->top() - stack_diff + 2;
  word stack_height = stack()->length() - frame_end;
  int bytecode_index =
      current_bcp + Bytecode::Size(opcode) - function->bytecode_address_for(0);
  return debug_info_->SetBreakpoint(function, bytecode_index, true, coroutine_,
                                    stack_height);
}

int Process::PrepareStepOut() {
  EnsureDebuggerAttached();
  Frame frame(stack());
  bool has_top_frame = frame.MovePrevious();
  ASSERT(has_top_frame);
  Object** frame_bottom = frame.FramePointer() + 1;
  Function* callee = frame.FunctionFromByteCodePointer();
  bool has_frame_below = frame.MovePrevious();
  ASSERT(has_frame_below);
  Function* caller = frame.FunctionFromByteCodePointer();
  int bytecode_index =
      frame.ByteCodePointer() - caller->bytecode_address_for(0);
  Object** expected_sp = frame_bottom + callee->arity();
  word frame_end = expected_sp - stack()->Pointer(0);
  word stack_height = stack()->length() - frame_end;
  return debug_info_->SetBreakpoint(caller, bytecode_index, true, coroutine_,
                                    stack_height);
}

void Process::CookStacks(int number_of_stacks) {
  cooked_stack_deltas_ = List<List<int>>::New(number_of_stacks);
  Object* raw_current = stack();
  for (int i = 0; i < number_of_stacks; ++i) {
    // TODO(ager): Space/time trade-off. Should we iterate the stack first
    // to count the number of frames to reduce memory pressure?
    Stack* current = Stack::cast(raw_current);
    cooked_stack_deltas_[i] = List<int>::New(stack()->length());
    int index = 0;
    Frame frame(current);
    while (frame.MovePrevious()) {
      uint8* bcp = frame.ByteCodePointer();
      Function* function = frame.FunctionFromByteCodePointer();
      uint8* start = function->bytecode_address_for(0);
      frame.SetByteCodePointer(reinterpret_cast<uint8*>(function));
      int delta = bcp - start;
      cooked_stack_deltas_[i][index++] = delta;
    }
    raw_current = current->next();
  }
  ASSERT(raw_current == Smi::zero());
}

void Process::UncookAndUnchainStacks() {
  Object* raw_current = stack();
  for (int i = 0; i < cooked_stack_deltas_.length(); ++i) {
    Stack* current = Stack::cast(raw_current);
    int index = 0;
    Frame frame(current);
    while (frame.MovePrevious()) {
      Object* value = reinterpret_cast<Object*>(frame.ByteCodePointer());
      ASSERT(value != NULL);
      Function* function = Function::cast(value);
      int delta = cooked_stack_deltas_[i][index++];
      uint8* bcp = function->bytecode_address_for(0) + delta;
      frame.SetByteCodePointer(bcp);
    }
    cooked_stack_deltas_[i].Delete();
    raw_current = current->next();
    current->set_next(Smi::FromWord(0));
  }
  ASSERT(raw_current == Smi::zero());
  cooked_stack_deltas_.Delete();
}

void Process::UpdateBreakpoints() {
  if (debug_info_ != NULL) {
    debug_info_->UpdateBreakpoints();
  }
}

void Process::RegisterFinalizer(HeapObject* object,
                                WeakPointerCallback callback) {
  uword address = object->address();
  if (heap()->space()->Includes(address)) {
    heap()->AddWeakPointer(object, callback);
  } else {
    ASSERT(immutable_heap()->space()->Includes(address));
    immutable_heap()->AddWeakPointer(object, callback);
  }
}

void Process::UnregisterFinalizer(HeapObject* object) {
  uword address = object->address();
  // We do not support unregistering weak pointers for the immutable heap (and
  // it is currently also not used for immutable objects).
  ASSERT(heap()->space()->Includes(address));
  heap()->RemoveWeakPointer(object);
}

void Process::FinalizeForeign(HeapObject* foreign, Heap* heap) {
  Instance* instance = Instance::cast(foreign);
  uword value = instance->GetConsecutiveSmis(0);
  uword length = Smi::cast(instance->GetInstanceField(2))->value();
  free(reinterpret_cast<void*>(value));
  heap->FreedForeignMemory(length);
}

void Process::FinalizeProcess(HeapObject* process, Heap* heap) {
  ProcessHandle* handle = ProcessHandle::FromDartObject(process);
  ProcessHandle::DecrementRef(handle);
}

#ifdef DEBUG
bool Process::TrueThenFalse() {
  bool result = true_then_false_;
  true_then_false_ = !true_then_false_;
  return result;
}
#endif

void Process::StoreErrno() { errno_cache_ = errno; }

void Process::RestoreErrno() { errno = errno_cache_; }

void Process::SendSignal(Signal* signal) {
  while (signal_.load() == NULL) {
    Signal* expected = NULL;
    if (signal_.compare_exchange_weak(expected, signal)) {
      return;
    }
  }
  Signal::DecrementRef(signal);
}

void Process::TakeChildHeaps() { mailbox_.MergeAllChildHeaps(this); }

void Process::UpdateStackLimit() {
  // By adding 2, we reserve a slot for a return address and an extra
  // temporary each bytecode can utilize internally.
  Stack* stack = this->stack();
  int frame_size = Bytecode::kGuaranteedFrameSize + 2;
  uword current_limit = stack_limit_;
  // Update the stack limit if the limit is a real limit or if all
  // interrupts have been handled.
  if (current_limit <= kMaxStackMarker) {
    uword new_stack_limit = reinterpret_cast<uword>(stack->Pointer(frame_size));
    stack_limit_.compare_exchange_strong(current_limit, new_stack_limit);
  }
}

LookupCache::Entry* Process::LookupEntrySlow(LookupCache::Entry* primary,
                                             Class* clazz, int selector) {
  ASSERT(!program()->is_compact());
  ThreadState* state = thread_state_;
  ASSERT(state != NULL);
  LookupCache* cache = state->cache();

  uword index = LookupCache::ComputeSecondaryIndex(clazz, selector);
  LookupCache::Entry* secondary = &(cache->secondary()[index]);
  if (secondary->clazz == clazz && secondary->selector == selector) {
    return secondary;
  }

  uword tag = 0;
  Function* target = clazz->LookupMethod(selector);
  if (target == NULL) {
    static const Names::Id name = Names::kNoSuchMethodTrampoline;
    target = clazz->LookupMethod(Selector::Encode(name, Selector::METHOD, 0));
  } else {
    void* intrinsic = target->ComputeIntrinsic(IntrinsicsTable::GetDefault());
    tag = (intrinsic == NULL) ? 1 : reinterpret_cast<uword>(intrinsic);
  }

  ASSERT(target != NULL);
  cache->DemotePrimary(primary);
  primary->clazz = clazz;
  primary->selector = selector;
  primary->target = target;
  primary->tag = tag;
  return primary;
}

NATIVE(ProcessQueueGetMessage) {
  MessageMailbox* mailbox = process->mailbox();

  Message* queue = mailbox->CurrentMessage();
  Message::Kind kind = queue->kind();
  Object* result = Smi::FromWord(0);

  switch (kind) {
    case Message::IMMEDIATE:
    case Message::IMMUTABLE_OBJECT:
      result = reinterpret_cast<Object*>(queue->value());
      break;

    case Message::FOREIGN:
    case Message::FOREIGN_FINALIZED: {
      Class* foreign_memory_class = process->program()->foreign_memory_class();
      ASSERT(foreign_memory_class->NumberOfInstanceFields() == 4);
      Object* object = process->NewInstance(foreign_memory_class);
      if (object == Failure::retry_after_gc()) return object;
      Instance* foreign = Instance::cast(object);
      foreign->SetConsecutiveSmis(0, queue->value());
      int size = queue->size();
      foreign->SetInstanceField(2, Smi::FromWord(size));
      if (kind == Message::FOREIGN_FINALIZED) {
        process->RegisterFinalizer(foreign, Process::FinalizeForeign);
        process->heap()->AllocatedForeignMemory(size);
      }
      result = foreign;
      break;
    }

    case Message::LARGE_INTEGER: {
      result = process->NewInteger(queue->value());
      if (result == Failure::retry_after_gc()) return result;
      break;
    }

    case Message::EXIT: {
      queue->MergeChildHeaps(process);
      result = queue->ExitReferenceObject();
      break;
    }

    case Message::PROCESS_DEATH_SIGNAL: {
      Program* program = process->program();

      Signal* signal = queue->ProcessDeathSignal();
      ProcessHandle* handle = signal->handle();

      Object* dart_process =
          process->NewInstance(program->process_class(), true);
      if (dart_process == Failure::retry_after_gc()) return dart_process;

      Object* process_death =
          process->NewInstance(program->process_death_class(), true);
      if (process_death == Failure::retry_after_gc()) return process_death;

      handle->IncrementRef();

      handle->InitializeDartObject(dart_process);
      Instance::cast(process_death)->SetInstanceField(0, dart_process);
      Instance::cast(process_death)
          ->SetInstanceField(1, Smi::FromWord(signal->kind()));

      process->RegisterFinalizer(HeapObject::cast(dart_process),
                                 Process::FinalizeProcess);

      result = process_death;
      break;
    }

    default:
      UNREACHABLE();
  }

  mailbox->AdvanceCurrentMessage();
  return result;
}

NATIVE(ProcessQueueGetChannel) {
  MessageMailbox* mailbox = process->mailbox();

  Message* queue = mailbox->CurrentMessage();
  // The channel for a port can die independently of the port. In that case
  // messages sent to the port can never be received. In that case we drop the
  // message when processing the message queue.
  while (queue != NULL) {
    Instance* channel = queue->port()->channel();
    if (channel != NULL) return channel;
    mailbox->AdvanceCurrentMessage();
    queue = mailbox->CurrentMessage();
  }
  return process->program()->null_object();
}

}  // namespace fletch
