import 'package:flutter/material.dart';

class Contact {
  final String id;
  final String name;
  final String handle;
  final String? status;
  final bool isOnline;
  final bool isPlaying;
  final Color? avatarColor;
  final String? memberSince;
  final int mutualServers;
  final int mutualFriends;

  Contact({
    required this.id,
    required this.name,
    required this.handle,
    this.status,
    this.isOnline = false,
    this.isPlaying = false,
    this.avatarColor,
    this.memberSince,
    this.mutualServers = 0,
    this.mutualFriends = 0,
  });
}
