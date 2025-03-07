// Copyright (c) 2014, the Fletch project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE.md file.

#ifndef SRC_VM_PROGRAM_H_
#define SRC_VM_PROGRAM_H_

#include "src/shared/globals.h"
#include "src/shared/random.h"
#include "src/vm/event_handler.h"
#include "src/vm/heap.h"
#include "src/vm/shared_heap.h"
#include "src/vm/links.h"
#include "src/vm/program_folder.h"

namespace fletch {

class Class;
class Function;
class Method;
class Process;
class ProcessVisitor;
class ProgramTableRewriter;
class Scheduler;
class Session;

// Defines all the roots in the program heap.
#define ROOTS_DO(V)                                             \
  V(Instance, null_object, NullObject)                          \
  V(Instance, false_object, FalseObject)                        \
  V(Instance, true_object, TrueObject)                          \
  /* Global literals up to this line */                         \
  V(Array, empty_array, EmptyArray)                             \
  V(OneByteString, empty_string, EmptyString)                   \
  V(Class, meta_class, MetaClass)                               \
  V(Class, smi_class, SmiClass)                                 \
  V(Class, boxed_class, BoxedClass)                             \
  V(Class, large_integer_class, LargeIntegerClass)              \
  V(Class, num_class, NumClass)                                 \
  V(Class, bool_class, BoolClass)                               \
  V(Class, int_class, IntClass)                                 \
  V(Class, one_byte_string_class, OneByteStringClass)           \
  V(Class, two_byte_string_class, TwoByteStringClass)           \
  V(Class, object_class, ObjectClass)                           \
  V(Class, array_class, ArrayClass)                             \
  V(Class, function_class, FunctionClass)                       \
  V(Class, closure_class, ClosureClass)                         \
  V(Class, byte_array_class, ByteArrayClass)                    \
  V(Class, double_class, DoubleClass)                           \
  V(Class, stack_class, StackClass)                             \
  V(Class, coroutine_class, CoroutineClass)                     \
  V(Class, process_class, ProcessClass)                         \
  V(Class, process_death_class, ProcessDeathClass)              \
  V(Class, port_class, PortClass)                               \
  V(Class, foreign_function_class, ForeignFunctionClass)        \
  V(Class, foreign_memory_class, ForeignMemoryClass)            \
  V(Class, initializer_class, InitializerClass)                 \
  V(Class, constant_list_class, ConstantListClass)              \
  V(Class, constant_byte_list_class, ConstantByteListClass)     \
  V(Class, constant_map_class, ConstantMapClass)                \
  V(Class, no_such_method_error_class, NoSuchMethodErrorClass)  \
  V(Class, stack_overflow_error_class, StackOverflowErrorClass) \
  V(HeapObject, stack_overflow_error, StackOverflowError)       \
  V(HeapObject, raw_retry_after_gc, RawRetryAfterGc)            \
  V(HeapObject, raw_wrong_argument_type, RawWrongArgumentType)  \
  V(HeapObject, raw_index_out_of_bounds, RawIndexOutOfBounds)   \
  V(HeapObject, raw_illegal_state, RawIllegalState)             \
  V(Object, native_failure_result, NativeFailureResult)         \
  V(Array, classes, Classes)                                    \
  V(Array, constants, Constants)                                \
  V(Array, static_methods, StaticMethods)                       \
  V(Array, static_fields, StaticFields)                         \
  V(Array, dispatch_table, DispatchTable)

class ProgramState {
 public:
  ProgramState() : paused_processes_head_(NULL), is_paused_(false) {}

  // The [Scheduler::pause_monitor_] must be locked when calling this method.
  void AddPausedProcess(Process* process);

  bool is_paused() const { return is_paused_; }
  void set_is_paused(bool value) { is_paused_ = value; }

  Process* paused_processes_head() const { return paused_processes_head_; }
  void set_paused_processes_head(Process* value) {
    paused_processes_head_ = value;
  }

 private:
  Process* paused_processes_head_;
  bool is_paused_;
};

class Program {
 public:
  enum ProgramSource {
    kLoadedFromSnapshot,
    kBuiltViaSession,
  };

  explicit Program(ProgramSource source);
  ~Program();

  void Initialize();

  // Is the program in the compact table representation?
  bool is_compact() const { return is_compact_; }
  void set_is_compact(bool value) { is_compact_ = value; }

  bool was_loaded_from_snapshot() { return loaded_from_snapshot_; }

  Function* entry() const { return entry_; }
  void set_entry(Function* entry) { entry_ = entry; }

  int main_arity() const { return main_arity_; }
  void set_main_arity(int value) { main_arity_ = value; }

  void set_classes(Array* classes) { classes_ = classes; }
  Class* class_at(int index) const { return Class::cast(classes_->get(index)); }

  void set_constants(Array* constants) { constants_ = constants; }
  Object* constant_at(int index) const { return constants_->get(index); }

  void set_static_methods(Array* static_methods) {
    static_methods_ = static_methods;
  }
  Function* static_method_at(int index) const {
    return Function::cast(static_methods_->get(index));
  }

  void set_static_fields(Array* static_fields) {
    static_fields_ = static_fields;
  }

  void set_dispatch_table(Array* dispatch_table) {
    dispatch_table_ = dispatch_table;
  }

  Scheduler* scheduler() const { return scheduler_; }
  void set_scheduler(Scheduler* scheduler) {
    ASSERT((scheduler_ == NULL && scheduler != NULL) ||
           (scheduler_ != NULL && scheduler == NULL));
    ASSERT(program_state_.paused_processes_head() == NULL);
    ASSERT(!program_state_.is_paused());
    scheduler_ = scheduler;
  }

  Signal::Kind exit_kind() const { return exit_kind_; }
  void set_exit_kind(Signal::Kind exit_kind) { exit_kind_ = exit_kind; }

  ProgramState* program_state() { return &program_state_; }

  EventHandler* event_handler() { return &event_handler_; }

  // TODO(ager): Support more than one active session at a time.
  void AddSession(Session* session) {
    ASSERT(session_ == NULL);
    session_ = session;
  }

  Session* session() { return session_; }

  Heap* heap() { return &heap_; }
  SharedHeap* shared_heap() { return &shared_heap_; }

  int program_heap_size() {
    ASSERT(is_compact_);
    Chunk* chunk = heap()->space()->first();
    ASSERT(chunk->next() == NULL);
    return chunk->limit() - chunk->base();
  }

  HeapObject* ObjectFromFailure(Failure* failure) {
    if (failure == Failure::wrong_argument_type()) {
      return raw_wrong_argument_type();
    } else if (failure == Failure::index_out_of_bounds()) {
      return raw_index_out_of_bounds();
    } else if (failure == Failure::illegal_state()) {
      return raw_illegal_state();
    }

    UNREACHABLE();
    return NULL;
  }

  Process* SpawnProcess(Process* parent);
  Process* ProcessSpawnForMain();
  // Returns [true] if this was the last process (i.e. main process).
  bool ScheduleProcessForDeletion(Process* process, Signal::Kind kind);

  // This function should only be called once the program has been stopped.
  void VisitProcesses(ProcessVisitor* visitor);

  Object* CreateArray(int capacity) {
    return CreateArrayWith(capacity, null_object());
  }
  Object* CreateArrayWith(int capacity, Object* initial_value);
  Object* CreateByteArray(int capacity);
  Object* CreateClass(int fields);
  Object* CreateDouble(fletch_double value);
  Object* CreateFunction(int arity, List<uint8> bytes, int number_of_literals);
  Object* CreateInteger(int64 value);
  Object* CreateLargeInteger(int64 value);
  Object* CreateStringFromAscii(List<const char> str);
  Object* CreateOneByteString(List<uint8> str);
  Object* CreateTwoByteString(List<uint16> str);
  Object* CreateInstance(Class* klass);
  Object* CreateInitializer(Function* function);

  void ValidateHeapsAreConsistent();
  void ValidateSharedHeap();

  void CollectGarbage();
  void CollectSharedGarbage(bool program_is_stopped = false);
  void PerformSharedGarbageCollection();
  void CompactStorebuffers();

  void PrintStatistics();

  // Iterates over all roots in the program.
  void IterateRoots(PointerVisitor* visitor);
  void IterateRootsIgnoringSession(PointerVisitor* visitor);

  // Dispatch table support.
  void ClearDispatchTableIntrinsics();
  void SetupDispatchTableIntrinsics(
      IntrinsicsTable* table = IntrinsicsTable::GetDefault());

  // Root objects.
 private:
#define DECLARE_ENUM(type, name, CamelName) k##CamelName##Index,
  enum { ROOTS_DO(DECLARE_ENUM) kNumberOfRoots };
#undef DECLARE_ENUM

 public:
#define ROOT_ACCESSOR(type, name, CamelName) \
  type* name() const { return name##_; }     \
  static const int k##CamelName##Offset = sizeof(void*) * k##CamelName##Index;
  ROOTS_DO(ROOT_ACCESSOR)
#undef ROOT_ACCESSOR

  RandomXorShift* random() { return &random_; }

  void PrepareProgramGC(bool disable_heap_validation_before_gc = false);
  void PerformProgramGC(Space* to, PointerVisitor* visitor);
  void FinishProgramGC();

  // When the program was loaded from a snapshot, then this function can be used
  // to get the offset of functions/classes in the program heap.
  uword OffsetOf(HeapObject* object);

 private:
  // Access to the address of the first and last root.
  Object** first_root_address() {
    return reinterpret_cast<Object**>(&null_object_);
  }

  Object** last_root_address() {
    return reinterpret_cast<Object**>(&dispatch_table_);
  }

  void ValidateGlobalHeapsAreConsistent();

  // Chaining of all processes of this program.
  void AddToProcessList(Process* process);
  void RemoveFromProcessList(Process* process);

#define ROOT_DECLARATION(type, name, CamelName) type* name##_;
  ROOTS_DO(ROOT_DECLARATION)
#undef ROOT_DECLARATION

  // Chained doubly linked list of all processes protected by a lock.
  Mutex* process_list_mutex_;
  Process* process_list_head_;

  RandomXorShift random_;

  Heap heap_;
  SharedHeap shared_heap_;

  Scheduler* scheduler_;
  ProgramState program_state_;

  EventHandler event_handler_;

  // Session operating on this program.
  Session* session_;

  Function* entry_;
  int main_arity_;

  bool is_compact_;

  bool loaded_from_snapshot_;

  Signal::Kind exit_kind_;
};

}  // namespace fletch

#endif  // SRC_VM_PROGRAM_H_
