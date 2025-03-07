// Copyright (c) 2015, the Fletch project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE.md file.

library fletch.debug_state;

import 'bytecodes.dart';
import 'fletch_system.dart';
import 'incremental/fletchc_incremental.dart';
import 'session.dart';
import 'src/debug_info.dart';
import 'src/class_debug_info.dart';

import 'commands.dart' show
    DartValue,
    InstanceStructure;

part 'stack_trace.dart';

/// A representation of a remote object.
abstract class RemoteObject {
  String name;

  RemoteObject(this.name);
}

/// A representation of a remote instance.
class RemoteInstance extends RemoteObject {
  /// An [InstanceStructure] describing the remote instance.
  final InstanceStructure instance;

  /// The fields as [DartValue]s of the remote instance.
  final List<DartValue> fields;

  RemoteInstance(this.instance, this.fields, {String name}) : super(name);
}

/// A representation of a remote primitive value (i.e. used for non-instances).
class RemoteValue extends RemoteObject {
  /// A [DartValue] describing the remote object.
  final DartValue value;

  RemoteValue(this.value, {String name}) : super(name);
}

class Breakpoint {
  final String methodName;
  final int bytecodeIndex;
  final int id;
  Breakpoint(this.methodName, this.bytecodeIndex, this.id);
  String toString() => "id: '$id' method: '$methodName' "
      "bytecode index: '$bytecodeIndex'";
}

class DebugState {
  final Session session;

  final Map<int, Breakpoint> breakpoints = <int, Breakpoint>{};
  final Map<FletchFunction, DebugInfo> debugInfos =
      <FletchFunction, DebugInfo>{};
  final Map<FletchClass, ClassDebugInfo> classDebugInfos =
      <FletchClass, ClassDebugInfo>{};

  bool showInternalFrames = false;
  bool verbose = true;
  StackFrame _topFrame;
  RemoteObject currentUncaughtException;
  StackTrace _currentStackTrace;
  int currentFrame = 0;
  SourceLocation _currentLocation;

  DebugState(this.session);

  void reset() {
    _topFrame = null;
    currentUncaughtException = null;
    _currentStackTrace = null;
    _currentLocation = null;
    currentFrame = 0;
  }

  int get actualCurrentFrameNumber {
    return currentStackTrace.actualFrameNumber(currentFrame);
  }

  ScopeInfo get currentScopeInfo {
    return currentStackTrace.scopeInfo(currentFrame);
  }

  SourceLocation get currentLocation => _currentLocation;

  StackTrace get currentStackTrace => _currentStackTrace;

  void set currentStackTrace(StackTrace stackTrace) {
    _currentLocation = stackTrace.sourceLocation();
    _topFrame = stackTrace.stackFrames[0];
    _currentStackTrace = stackTrace;
  }

  StackFrame get topFrame => _topFrame;

  void set topFrame(StackFrame frame) {
    _currentLocation = frame.sourceLocation();
    _topFrame = frame;
  }

  DebugInfo getDebugInfo(FletchFunction function) {
    return debugInfos.putIfAbsent(function, () {
      return session.compiler.createDebugInfo(function, session.fletchSystem);
    });
  }

  ClassDebugInfo getClassDebugInfo(FletchClass klass) {
    return classDebugInfos.putIfAbsent(klass, () {
      return session.compiler.createClassDebugInfo(klass);
    });
  }

  String lookupFieldName(FletchClass klass, int field) {
    while (field < klass.superclassFields) {
      klass = session.fletchSystem.lookupClassById(klass.superclassId);
    }
    return getClassDebugInfo(klass).fieldNames[field - klass.superclassFields];
  }

  bool atLocation(SourceLocation previous) {
    return (!topFrame.isVisible ||
            currentLocation == null ||
            currentLocation.isSameSourceLevelLocationAs(previous) ||
            currentLocation.node == null);
  }

  int get numberOfStackFrames => currentStackTrace.stackFrames.length;

  SourceLocation sourceLocationForFrame(int frame) {
    return currentStackTrace.stackFrames[frame].sourceLocation();
  }
}
