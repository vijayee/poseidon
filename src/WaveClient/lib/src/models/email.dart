class Email {
  final String id;
  final String senderName;
  final String senderEmail;
  final String subject;
  final String preview;
  final String time;
  final bool hasAttachment;
  final int attachmentCount;
  final bool isRead;
  final String? avatarUrl;

  Email({
    required this.id,
    required this.senderName,
    required this.senderEmail,
    required this.subject,
    required this.preview,
    required this.time,
    this.hasAttachment = false,
    this.attachmentCount = 0,
    this.isRead = false,
    this.avatarUrl,
  });
}
