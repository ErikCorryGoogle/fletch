// Copyright (c) 2015, the Fletch project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE.md file.

part of fletch.debug_state;

class StackFrame {
  final FletchFunction function;
  final int bytecodePointer;
  final IncrementalCompiler compiler;
  final DebugState debugState;

  StackFrame(this.function, this.bytecodePointer, this.compiler,
             this.debugState);

  bool get inPlatformLibrary => function.element.library.isPlatformLibrary;

  bool get isInternal => function.isInternal || inPlatformLibrary;

  String invokeString(Bytecode bytecode) {
    if (bytecode is InvokeMethod) {
      String name =
          compiler.lookupFunctionNameBySelector(bytecode.uint32Argument0);
      return ' ($name)';
    }
    return '';
  }

  bool get isVisible => debugState.showInternalFrames || !isInternal;

  DebugInfo get debugInfo => debugState.getDebugInfo(function);

  String list({int contextLines: 5}) {
    return debugInfo.sourceListStringFor(
        bytecodePointer - 1, contextLines: contextLines);
  }

  String disasm() {
    StringBuffer buffer = new StringBuffer();
    var bytecodes = function.bytecodes;
    var offset = 0;
    for (var i = 0; i  < bytecodes.length; i++) {
      var source = debugInfo.astStringFor(offset);
      var current = bytecodes[i];
      var byteNumberString = '$offset'.padLeft(4);
      var invokeInfo = invokeString(current);
      var bytecodeString = '$byteNumberString $current$invokeInfo';
      var sourceString = '// $source';
      var printString = bytecodeString.padRight(30) + sourceString;
      offset += current.size;
      var marker = (offset == bytecodePointer) ? '>' : ' ';
      buffer.writeln("  $marker$printString");
    }
    buffer.writeln('');
    return buffer.toString();
  }

  String shortString([int namePadding = 0]) {
    String name = compiler.lookupFunctionName(function);
    String astString = debugInfo.astStringFor(bytecodePointer - 1);
    astString = (astString != null) ? '@$astString' : '';

    String paddedName = name.padRight(namePadding);
    String spaces = '';
    if (astString.isNotEmpty) {
      int missingSpaces = 4 - (paddedName.length % 4);
      spaces = ' ' * missingSpaces;
    }

    return '$paddedName$spaces$astString';
  }

  SourceLocation sourceLocation() {
    return debugInfo.sourceLocationFor(bytecodePointer - 1);
  }

  ScopeInfo scopeInfo() {
    return debugInfo.scopeInfoFor(bytecodePointer - 1);
  }

  bool isSameSourceLocation(int offset,
                            SourceLocation current) {
    SourceLocation location = debugInfo.sourceLocationFor(offset);
    // Treat locations for which we have no source information as the same
    // as the previous location.
    if (location == null || location.node == null) return true;
    return location.isSameSourceLevelLocationAs(current);
  }

  int stepBytecodePointer(SourceLocation current) {
    var bytecodes = function.bytecodes;
    // Zip forward to the current bytecode. The bytecode pointer in the stack
    // frame is the return address which is one bytecode after the current one.
    var offset = 0;
    var i = 0;
    for (; i < bytecodes.length; i++) {
      var currentSize = bytecodes[i].size;
      if (offset + currentSize == bytecodePointer) break;
      offset += currentSize;
    }
    // Move forward while we know step should not stop.
    while (!bytecodes[i].isBranching &&
           isSameSourceLocation(offset, current)) {
      offset += bytecodes[i++].size;
    }
    return offset <= bytecodePointer ? -1 : offset;
  }

  int get functionId => function.functionId;
}

class StackTrace {
  final List<StackFrame> stackFrames;
  final DebugState debugState;

  List<int> visibleFrameMapping;
  int framesToGo;
  int maxNameLength = 0;

  StackTrace(int framesToGo, this.debugState)
      : this.framesToGo = framesToGo,
        stackFrames = new List(framesToGo);

  int get frames => stackFrames.length;

  int get visibleFrames {
    ensureVisibleFrameMap();
    return visibleFrameMapping.length;
  }

  void addFrame(IncrementalCompiler compiler, StackFrame frame) {
    stackFrames[--framesToGo] = frame;
    String name = compiler.lookupFunctionName(frame.function);
    var nameLength = name == null ? 0 : name.length;
    if (nameLength > maxNameLength) maxNameLength = nameLength;
  }

  String format([int frame]) {
    int currentFrame = frame != null ? frame : debugState.currentFrame;
    StringBuffer buffer = new StringBuffer();
    assert(framesToGo == 0);
    var frameNumber = 0;
    for (var i = 0; i < stackFrames.length; i++) {
      if (!stackFrames[i].isVisible) continue;
      if (frameNumber == 0) buffer.writeln("Stack trace:");
      var marker = currentFrame == frameNumber ? '> ' : '  ';
      var line = stackFrames[i].shortString(maxNameLength);
      String frameNumberString = '${frameNumber++}: '.padLeft(3);
      buffer.writeln('$marker$frameNumberString$line');
    }
    return buffer.toString();
  }

  void ensureVisibleFrameMap() {
    if (visibleFrameMapping == null) {
      visibleFrameMapping = [];
      for (int i = 0; i < stackFrames.length; i++) {
        if (stackFrames[i].isVisible) visibleFrameMapping.add(i);
      }
    }
  }

  // Map user visible frame numbers to actual frame numbers.
  int actualFrameNumber(int visibleFrameNumber) {
    ensureVisibleFrameMap();
    return (visibleFrameNumber < visibleFrameMapping.length)
        ? visibleFrameMapping[visibleFrameNumber]
        : -1;
  }

  StackFrame visibleFrame(int frame) {
    int frameNumber = actualFrameNumber(frame);
    if (frameNumber == -1) return null;
    return stackFrames[frameNumber];
  }

  void visibilityChanged() {
    visibleFrameMapping = null;
  }

  String list([int frame]) {
    if (frame == null) frame = debugState.currentFrame;
    StackFrame visibleStackFrame = visibleFrame(frame);
    if (visibleStackFrame == null) return null;
    return visibleStackFrame.list();
  }

  String disasm([int frame]) {
    if (frame == null) frame = debugState.currentFrame;
    StackFrame visibleStackFrame = visibleFrame(frame);
    if (visibleStackFrame == null) return null;
    return visibleStackFrame.disasm();
  }

  SourceLocation sourceLocation() {
    return stackFrames[0].sourceLocation();
  }

  ScopeInfo scopeInfo(int frame) {
    StackFrame visibleStackFrame = visibleFrame(frame);
    if (visibleStackFrame == null) return null;
    return visibleStackFrame.scopeInfo();
  }

  ScopeInfo get scopeInfoForCurrentFrame => scopeInfo(debugState.currentFrame);

  int stepBytecodePointer(SourceLocation location) {
    return stackFrames[0].stepBytecodePointer(location);
  }
}
