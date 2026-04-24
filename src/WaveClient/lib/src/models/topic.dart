class Topic {
  final String id;
  final String name;
  final String? description;
  final bool isVoice;
  final bool isActive;

  Topic({
    required this.id,
    required this.name,
    this.description,
    this.isVoice = false,
    this.isActive = false,
  });
}
