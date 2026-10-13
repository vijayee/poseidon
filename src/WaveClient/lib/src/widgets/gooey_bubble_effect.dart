import 'dart:math';
import 'dart:ui';

import 'package:flutter/material.dart';

/// A cluster of small circles that travel together from the input bar to the
/// message area.  Their overlap plus a moderate blur approximates the
/// SVG-gooey "merging blobs" look.
class _GooeyCluster {
  final List<_Blob> blobs;
  final Offset start;
  final Offset end;
  final DateTime startTime;
  final Duration duration;

  _GooeyCluster({
    required this.blobs,
    required this.start,
    required this.end,
    required this.duration,
  }) : startTime = DateTime.now();
}

class _Blob {
  final double dx;
  final double dy;
  final double baseRadius;
  final double phase;

  _Blob({
    required this.dx,
    required this.dy,
    required this.baseRadius,
    required this.phase,
  });
}

class _GooeyPainter extends CustomPainter {
  final List<_GooeyCluster> clusters;
  final Color color;

  _GooeyPainter(this.clusters, this.color);

  @override
  void paint(Canvas canvas, Size size) {
    final now = DateTime.now();
    for (final cluster in clusters) {
      final elapsed = now.difference(cluster.startTime).inMilliseconds;
      final total = cluster.duration.inMilliseconds;
      final p = (elapsed / total).clamp(0.0, 1.0);
      if (p >= 1.0) continue;

      // Global position: Sine ease-in-out for Y
      final yP = _sineEaseInOut(p);
      final globalY = cluster.start.dy + (cluster.end.dy - cluster.start.dy) * yP;

      // Global X: Quad ease-in-out with 0.2s delay
      double xP;
      if (p < 0.25) {
        xP = 0;
      } else {
        xP = _quadEaseInOut((p - 0.25) / 0.75);
      }
      final globalX = cluster.start.dx + (cluster.end.dx - cluster.start.dx) * xP;

      // Opacity: hold until ~0.65s, then fade
      double opacity;
      if (p > 0.81) {
        opacity = 1.0 - ((p - 0.81) / 0.19).clamp(0.0, 1.0);
      } else {
        opacity = 1.0;
      }

      for (final blob in cluster.blobs) {
        // Each blob oscillates slightly around the cluster centre
        final osc = sin((p * 2 * pi) + blob.phase);
        final bx = globalX + blob.dx + osc * 6;
        final by = globalY + blob.dy + cos((p * 2 * pi) + blob.phase) * 4;

        final paint = Paint()
          ..color = color.withValues(alpha: opacity * 0.9)
          ..style = PaintingStyle.fill
          ..imageFilter = ImageFilter.blur(
            sigmaX: 5,
            sigmaY: 5,
            tileMode: TileMode.clamp,
          );

        canvas.drawCircle(Offset(bx, by), blob.baseRadius, paint);
      }
    }
  }

  double _sineEaseInOut(double t) => -0.5 * (cos(t * pi) - 1);

  double _quadEaseInOut(double t) {
    if (t < 0.5) return 2 * t * t;
    return -1 + (4 - 2 * t) * t;
  }

  @override
  bool shouldRepaint(covariant _GooeyPainter oldDelegate) => true;
}

/// Overlay widget that shows gooey message transitions.
class GooeyBubbleEffect extends StatefulWidget {
  final Widget child;
  final Color bubbleColor;

  const GooeyBubbleEffect({
    super.key,
    required this.child,
    this.bubbleColor = const Color(0xFF32A8E6),
  });

  @override
  State<GooeyBubbleEffect> createState() => GooeyBubbleEffectState();
}

class GooeyBubbleEffectState extends State<GooeyBubbleEffect>
    with SingleTickerProviderStateMixin {
  final List<_GooeyCluster> _clusters = [];
  late AnimationController _controller;

  @override
  void initState() {
    super.initState();
    _controller = AnimationController(
      vsync: this,
      duration: const Duration(seconds: 2),
    );
    _controller.addListener(() {
      setState(() {});
      _clusters.removeWhere((c) {
        final elapsed = DateTime.now().difference(c.startTime).inMilliseconds;
        return elapsed >= c.duration.inMilliseconds;
      });
    });
  }

  void spawnTransition({
    required Offset start,
    required Offset end,
  }) {
    // Spawn a cluster of 5 overlapping blobs
    final blobs = <_Blob>[
      _Blob(dx: 0, dy: 0, baseRadius: 18, phase: 0),
      _Blob(dx: -8, dy: 4, baseRadius: 14, phase: 1.2),
      _Blob(dx: 8, dy: -3, baseRadius: 14, phase: 2.5),
      _Blob(dx: -4, dy: -6, baseRadius: 12, phase: 3.7),
      _Blob(dx: 6, dy: 6, baseRadius: 12, phase: 4.9),
    ];

    _clusters.add(_GooeyCluster(
      blobs: blobs,
      start: start,
      end: end,
      duration: const Duration(milliseconds: 800),
    ));
    _controller.forward(from: 0);
  }

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Stack(
      children: [
        widget.child,
        IgnorePointer(
          child: CustomPaint(
            size: Size.infinite,
            painter: _GooeyPainter(
              List.unmodifiable(_clusters),
              widget.bubbleColor,
            ),
          ),
        ),
      ],
    );
  }
}
