// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:typed_data';
import 'dart:ui';

void addNode(
  SemanticsUpdateBuilder builder, {
  required int id,
  required String label,
  required double x,
  required double y,
  required double width,
  required double height,
  List<int> children = const <int>[],
  bool tappable = false,
}) {
  final transform = Float64List.fromList(<double>[1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, 0, 1]);
  builder.updateNode(
    id: id,
    flags: SemanticsFlags(isButton: tappable),
    actions: tappable ? SemanticsAction.tap.index : 0,
    maxValueLength: 0,
    currentValueLength: 0,
    textSelectionBase: -1,
    textSelectionExtent: -1,
    platformViewId: -1,
    scrollChildren: 0,
    scrollIndex: 0,
    traversalParent: 0,
    scrollPosition: 0,
    scrollExtentMax: 0,
    scrollExtentMin: 0,
    rect: Rect.fromLTWH(0, 0, width, height),
    identifier: '',
    label: label,
    labelAttributes: const <StringAttribute>[],
    value: '',
    valueAttributes: const <StringAttribute>[],
    increasedValue: '',
    increasedValueAttributes: const <StringAttribute>[],
    decreasedValue: '',
    decreasedValueAttributes: const <StringAttribute>[],
    hint: '',
    hintAttributes: const <StringAttribute>[],
    tooltip: '',
    textDirection: TextDirection.ltr,
    transform: transform,
    hitTestTransform: transform,
    childrenInTraversalOrder: Int32List.fromList(children),
    childrenInHitTestOrder: Int32List.fromList(children),
    additionalActions: Int32List(0),
    controlsNodes: null,
    inputType: SemanticsInputType.none,
    locale: null,
    minValue: '',
    maxValue: '',
  );
}

void main() {
  final dispatcher = PlatformDispatcher.instance;
  var actionReceived = false;

  void sendSemantics() {
    final view = dispatcher.implicitView;
    if (!dispatcher.semanticsEnabled || view == null) {
      return;
    }
    dispatcher.setSemanticsTreeEnabled(true);
    final builder = SemanticsUpdateBuilder();
    addNode(
      builder,
      id: 0,
      label: 'engine-root',
      x: 7,
      y: 13,
      width: 200,
      height: 130,
      children: const <int>[7],
    );
    addNode(
      builder,
      id: 7,
      label: actionReceived ? 'action-received' : 'engine-child',
      x: 11,
      y: 13,
      width: 60,
      height: 40,
      tappable: true,
    );
    view.updateSemantics(builder.build());
  }

  dispatcher.onSemanticsEnabledChanged = sendSemantics;
  dispatcher.onMetricsChanged = sendSemantics;
  dispatcher.onSemanticsActionEvent = (SemanticsActionEvent event) {
    if (event.viewId == dispatcher.implicitView?.viewId &&
        event.nodeId == 7 &&
        event.type == SemanticsAction.tap) {
      actionReceived = true;
      sendSemantics();
    }
  };
  dispatcher.onBeginFrame = (Duration _) {
    final view = dispatcher.implicitView;
    if (view == null) {
      return;
    }
    final recorder = PictureRecorder();
    Canvas(recorder)
        .drawRect(const Rect.fromLTWH(0, 0, 280, 180), Paint()..color = const Color(0xff164b74));
    final picture = recorder.endRecording();
    final scene = (SceneBuilder()..addPicture(Offset.zero, picture)).build();
    view.render(scene);
    scene.dispose();
    picture.dispose();
    sendSemantics();
  };
  dispatcher.scheduleFrame();
  sendSemantics();
}
