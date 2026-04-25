import 'package:flutter/material.dart';
import 'package:flutter_svg/flutter_svg.dart';
import 'package:google_fonts/google_fonts.dart';
import 'src/theme/app_theme.dart';
import 'src/models/email.dart';
import 'src/models/topic.dart';
import 'src/models/contact.dart';
import 'src/widgets/email_card.dart';

void main() {
  runApp(const WaveApp());
}

class WaveApp extends StatelessWidget {
  const WaveApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Poseidon Wave',
      theme: AppTheme.lightTheme,
      debugShowCheckedModeBanner: false,
      home: const EmailClientScreen(),
    );
  }
}

enum ViewMode { mail, chat }
enum MailSubView { inbox, conversations }

class EmailClientScreen extends StatefulWidget {
  const EmailClientScreen({super.key});

  @override
  State<EmailClientScreen> createState() => _EmailClientScreenState();
}

class _EmailClientScreenState extends State<EmailClientScreen> {
  ViewMode _viewMode = ViewMode.mail;
  MailSubView _mailSubView = MailSubView.inbox;
  int _selectedEmailIndex = 0;
  int _selectedChannelIndex = -1;
  int _selectedTopicIndex = 0;
  int _selectedContactIndex = 0;

  final List<Email> _emails = [
    Email(
      id: '1',
      senderName: 'Mark Conners',
      senderEmail: 'mark@example.com',
      subject: 'Hey there! Check out thi...',
      preview: 'Leverage agile frameworks to provide a robust synopsis for high level overviews...',
      time: 'Apr 8, 2018',
      hasAttachment: true,
      attachmentCount: 1,
    ),
    Email(
      id: '2',
      senderName: 'Lily Jones',
      senderEmail: 'lily@example.com',
      subject: 'Invitation',
      preview: "We'd like to invite you to our annual meeting in San Francisco...",
      time: 'Feb 5, 2018',
    ),
    Email(
      id: '3',
      senderName: 'Nicole Adams',
      senderEmail: 'nicole@example.com',
      subject: '2018 Summer Sale!',
      preview: "The biggest sale in this year! We're introducing new products with discounts up to 50%...",
      time: 'Feb 12, 2018',
    ),
    Email(
      id: '4',
      senderName: 'Marie Clark',
      senderEmail: 'marie@example.com',
      subject: 'Job Proposal',
      preview: "Hey there Michael! I'm a huge fan of your work. I have this project that needs your expertise...",
      time: 'Jan 22, 2018',
    ),
    Email(
      id: '5',
      senderName: 'Mark Conners',
      senderEmail: 'mark@example.com',
      subject: 'Web Design Quote',
      preview: 'Dear Mitch and Tash, here is a proposal for your project. Feel free to discuss any time...',
      time: 'Dec 20, 2017',
    ),
  ];

  final List<List<Topic>> _channelTopics = [
    [
      Topic(id: 'c1', name: 'general', description: 'General discussion', isActive: true),
      Topic(id: 'c2', name: 'random', description: 'Off-topic chatter'),
      Topic(id: 'c3', name: 'announcements', description: 'Server announcements'),
      Topic(id: 'c4', name: 'help', description: 'Get help here'),
    ],
    [
      Topic(id: 'c1', name: 'general', description: 'General discussion', isActive: true),
      Topic(id: 'c2', name: 'design', description: 'Design discussions'),
      Topic(id: 'c3', name: 'development', description: 'Dev talk'),
    ],
    [
      Topic(id: 'c1', name: 'general', description: 'General discussion', isActive: true),
      Topic(id: 'c2', name: 'lounge', description: 'Hangout spot'),
    ],
  ];

  final List<String> _channelNames = ['Team Alpha', 'Design Hub', 'Dev Central'];

  final List<Contact> _contacts = [
    Contact(
      id: 'c1',
      name: 'Nicole Adams',
      handle: 'nicole_adams',
      status: 'Working on designs',
      isOnline: true,
      avatarColor: Colors.pink,
      memberSince: 'Jun 1, 2016',
      mutualServers: 1,
      mutualFriends: 2,
    ),
    Contact(
      id: 'c2',
      name: 'Calo',
      handle: 'calo_design',
      status: 'Playing League of Legends',
      isOnline: true,
      isPlaying: true,
      avatarColor: Colors.purple,
      memberSince: 'Mar 15, 2018',
      mutualServers: 3,
      mutualFriends: 5,
    ),
    Contact(
      id: 'c3',
      name: 'iceberg_ssj',
      handle: 'iceberg_ssj',
      isOnline: true,
      avatarColor: Colors.green,
      memberSince: 'Dec 8, 2019',
      mutualServers: 1,
      mutualFriends: 0,
    ),
    Contact(
      id: 'c4',
      name: 'Superman',
      handle: 'kapture_superman',
      isOnline: true,
      avatarColor: Colors.blue,
      memberSince: 'Jan 20, 2015',
      mutualServers: 2,
      mutualFriends: 4,
    ),
    Contact(
      id: 'c5',
      name: 'Lily Jones',
      handle: 'lily_jones',
      isOnline: false,
      avatarColor: Colors.grey,
      memberSince: 'Aug 3, 2020',
      mutualServers: 1,
      mutualFriends: 1,
    ),
    Contact(
      id: 'c6',
      name: 'Marie Clark',
      handle: 'marie_clark',
      isOnline: false,
      avatarColor: Colors.grey,
      memberSince: 'Nov 12, 2017',
      mutualServers: 0,
      mutualFriends: 2,
    ),
  ];

  void _switchToMail() {
    setState(() {
      _viewMode = ViewMode.mail;
      _selectedChannelIndex = -1;
    });
  }

  void _switchToChat(int channelIndex) {
    setState(() {
      _viewMode = ViewMode.chat;
      _selectedChannelIndex = channelIndex;
      _selectedTopicIndex = 0;
    });
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: AppColors.background,
      body: Column(
        children: [
          // Global Top Bar - adapts based on view mode
          _buildTopBar(),
          // Content Area
          Expanded(
            child: Row(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                ChannelRail(
                  selectedChannel: _selectedChannelIndex,
                  onMailTap: _switchToMail,
                  onChannelTap: _switchToChat,
                ),
                if (_viewMode == ViewMode.mail) ...[
                  LeftSidebar(
                    selectedSubView: _mailSubView,
                    onSubViewSelect: (subView) => setState(() => _mailSubView = subView),
                  ),
                  if (_mailSubView == MailSubView.inbox) ...[
                    EmailListPane(
                      emails: _emails,
                      selectedIndex: _selectedEmailIndex,
                      onSelect: (index) => setState(() => _selectedEmailIndex = index),
                    ),
                    Expanded(
                      flex: 3,
                      child: EmailDetailPane(email: _emails[_selectedEmailIndex]),
                    ),
                    ContactStatusPanel(contacts: _contacts),
                  ] else ...[
                    ContactListPane(
                      contacts: _contacts,
                      selectedIndex: _selectedContactIndex,
                      onSelect: (index) => setState(() => _selectedContactIndex = index),
                    ),
                    Expanded(
                      flex: 3,
                      child: ChatArea(
                        topicName: _contacts[_selectedContactIndex].name,
                      ),
                    ),
                    SingleContactPanel(contact: _contacts[_selectedContactIndex]),
                  ],
                ] else ...[
                  TopicList(
                    channelName: _channelNames[_selectedChannelIndex],
                    topics: _channelTopics[_selectedChannelIndex],
                    selectedIndex: _selectedTopicIndex,
                    onSelect: (index) => setState(() => _selectedTopicIndex = index),
                  ),
                  Expanded(
                    child: ChatArea(
                      topicName: _channelTopics[_selectedChannelIndex][_selectedTopicIndex].name,
                    ),
                  ),
                  const MembersPanel(),
                ],
              ],
            ),
          ),
        ],
      ),
    );
  }

  Widget _buildTopBar() {
    if (_viewMode == ViewMode.mail) {
      return _buildMailTopBar();
    }
    return _buildChatTopBar();
  }

  Widget _buildMailTopBar() {
    return Container(
      height: 64,
      decoration: const BoxDecoration(
        color: AppColors.surface,
        border: Border(
          bottom: BorderSide(color: AppColors.border),
        ),
      ),
      child: Row(
        children: [
          // Column 1: ChannelRail width (72px) - Logo
          SizedBox(
            width: 72,
            child: Center(
              child: SizedBox(
                width: 48,
                height: 48,
                child: SvgPicture.asset(
                  'assets/poseidon_logo.svg',
                ),
              ),
            ),
          ),
          // Column 2: LeftSidebar width (200px) - Poseidon text
          SizedBox(
            width: 200,
            child: Padding(
              padding: const EdgeInsets.only(left: 16),
              child: Text(
                'Poseidon',
                style: GoogleFonts.leagueSpartan(
                  fontWeight: FontWeight.w700,
                  fontSize: 20,
                  color: AppColors.textPrimary,
                ),
              ),
            ),
          ),
          // Column 3: EmailListPane width (340px) - Sort controls
          SizedBox(
            width: 340,
            child: Row(
              mainAxisAlignment: MainAxisAlignment.end,
              children: [
                const Text(
                  'Sort',
                  style: TextStyle(fontSize: 13, color: AppColors.textSecondary),
                ),
                const SizedBox(width: 12),
                Container(
                  padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 4),
                  decoration: BoxDecoration(
                    color: AppColors.background,
                    borderRadius: BorderRadius.circular(6),
                    border: Border.all(color: AppColors.border),
                  ),
                  child: const Row(
                    children: [
                      Text(
                        'Newest First',
                        style: TextStyle(fontSize: 13, color: AppColors.textSecondary),
                      ),
                      SizedBox(width: 4),
                      Icon(Icons.keyboard_arrow_down, size: 18, color: AppColors.textSecondary),
                    ],
                  ),
                ),
              ],
            ),
          ),
          // Column 4: EmailDetailPane (Expanded/flex 3) - Search bar
          Expanded(
            flex: 3,
            child: Padding(
              padding: const EdgeInsets.symmetric(horizontal: 16),
              child: Container(
                padding: const EdgeInsets.symmetric(horizontal: 16),
                decoration: BoxDecoration(
                  color: AppColors.background,
                  borderRadius: BorderRadius.circular(24),
                  border: Border.all(color: AppColors.border),
                ),
                child: const Row(
                  children: [
                    Icon(Icons.search, size: 18, color: AppColors.textMuted),
                    SizedBox(width: 10),
                    Expanded(
                      child: TextField(
                        decoration: InputDecoration(
                          hintText: 'Search your mail...',
                          hintStyle: TextStyle(fontSize: 13, color: AppColors.textMuted),
                          border: InputBorder.none,
                          isDense: true,
                          contentPadding: EdgeInsets.symmetric(vertical: 12),
                        ),
                      ),
                    ),
                  ],
                ),
              ),
            ),
          ),
          // Column 5: ContactStatusPanel width (280px) - Notifications + User
          SizedBox(
            width: 280,
            child: Padding(
              padding: const EdgeInsets.symmetric(horizontal: 16),
              child: Row(
                mainAxisAlignment: MainAxisAlignment.end,
                children: [
                  Stack(
                    children: [
                      const Icon(Icons.notifications_none, size: 24, color: AppColors.textSecondary),
                      Positioned(
                        right: 0,
                        top: 0,
                        child: Container(
                          width: 8,
                          height: 8,
                          decoration: const BoxDecoration(
                            color: Colors.red,
                            shape: BoxShape.circle,
                          ),
                        ),
                      ),
                    ],
                  ),
                  const SizedBox(width: 16),
                  const CircleAvatar(
                    radius: 16,
                    backgroundColor: Colors.orange,
                    child: Text('M', style: TextStyle(color: Colors.white, fontSize: 12, fontWeight: FontWeight.bold)),
                  ),
                  const SizedBox(width: 8),
                  const Text(
                    'Michael Williams',
                    style: TextStyle(
                      fontSize: 13,
                      fontWeight: FontWeight.w500,
                      color: AppColors.textPrimary,
                    ),
                  ),
                ],
              ),
            ),
          ),
        ],
      ),
    );
  }

  Widget _buildChatTopBar() {
    return Container(
      height: 64,
      decoration: const BoxDecoration(
        color: AppColors.surface,
        border: Border(
          bottom: BorderSide(color: AppColors.border),
        ),
      ),
      child: Row(
        children: [
          // Column 1: ChannelRail width (72px) - Logo
          SizedBox(
            width: 72,
            child: Center(
              child: SizedBox(
                width: 48,
                height: 48,
                child: SvgPicture.asset(
                  'assets/poseidon_logo.svg',
                ),
              ),
            ),
          ),
          // Column 2: TopicList width (200px) - Poseidon text
          SizedBox(
            width: 200,
            child: Padding(
              padding: const EdgeInsets.only(left: 16),
              child: Text(
                'Poseidon',
                style: GoogleFonts.leagueSpartan(
                  fontWeight: FontWeight.w700,
                  fontSize: 20,
                  color: AppColors.textPrimary,
                ),
              ),
            ),
          ),
          // Column 3-4: Chat area header - Channel name + search
          Expanded(
            child: Padding(
              padding: const EdgeInsets.symmetric(horizontal: 16),
              child: Row(
                children: [
                  Expanded(
                    child: Container(
                      padding: const EdgeInsets.symmetric(horizontal: 16),
                      decoration: BoxDecoration(
                        color: AppColors.background,
                        borderRadius: BorderRadius.circular(24),
                        border: Border.all(color: AppColors.border),
                      ),
                      child: const Row(
                        children: [
                          Icon(Icons.search, size: 18, color: AppColors.textMuted),
                          SizedBox(width: 10),
                          Expanded(
                            child: TextField(
                              decoration: InputDecoration(
                                hintText: 'Search...',
                                hintStyle: TextStyle(fontSize: 13, color: AppColors.textMuted),
                                border: InputBorder.none,
                                isDense: true,
                                contentPadding: EdgeInsets.symmetric(vertical: 12),
                              ),
                            ),
                          ),
                        ],
                      ),
                    ),
                  ),
                  const SizedBox(width: 16),
                  const Icon(Icons.mail_outline, size: 22, color: AppColors.textSecondary),
                  const SizedBox(width: 16),
                  const Icon(Icons.people_outline, size: 22, color: AppColors.textSecondary),
                ],
              ),
            ),
          ),
          // Column 5: MembersPanel width (280px) - Notifications + User
          SizedBox(
            width: 280,
            child: Padding(
              padding: const EdgeInsets.symmetric(horizontal: 16),
              child: Row(
                mainAxisAlignment: MainAxisAlignment.end,
                children: [
                  Stack(
                    children: [
                      const Icon(Icons.notifications_none, size: 24, color: AppColors.textSecondary),
                      Positioned(
                        right: 0,
                        top: 0,
                        child: Container(
                          width: 8,
                          height: 8,
                          decoration: const BoxDecoration(
                            color: Colors.red,
                            shape: BoxShape.circle,
                          ),
                        ),
                      ),
                    ],
                  ),
                  const SizedBox(width: 16),
                  const CircleAvatar(
                    radius: 16,
                    backgroundColor: Colors.orange,
                    child: Text('M', style: TextStyle(color: Colors.white, fontSize: 12, fontWeight: FontWeight.bold)),
                  ),
                  const SizedBox(width: 8),
                  const Text(
                    'Michael Williams',
                    style: TextStyle(
                      fontSize: 13,
                      fontWeight: FontWeight.w500,
                      color: AppColors.textPrimary,
                    ),
                  ),
                ],
              ),
            ),
          ),
        ],
      ),
    );
  }
}

class ChannelRail extends StatelessWidget {
  final int selectedChannel;
  final VoidCallback onMailTap;
  final Function(int) onChannelTap;

  const ChannelRail({
    super.key,
    required this.selectedChannel,
    required this.onMailTap,
    required this.onChannelTap,
  });

  @override
  Widget build(BuildContext context) {
    return Container(
      width: 72,
      color: const Color(0xFFF8F9FA),
      child: Column(
        children: [
          const SizedBox(height: 12),
          // Mail icon - replaces PW bubble
          _buildMailIcon(),
          Padding(
            padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
            child: Container(height: 2, color: const Color(0xFFE8E8E8)),
          ),
          _buildServerIcon(
            label: 'T1',
            color: Colors.grey,
            index: 0,
          ),
          _buildServerIcon(
            label: 'T2',
            color: Colors.blueGrey,
            index: 1,
          ),
          _buildServerIcon(
            label: 'T3',
            color: Colors.indigo,
            index: 2,
          ),
          _buildServerIcon(label: '+', color: Colors.transparent, isAdd: true),
        ],
      ),
    );
  }

  Widget _buildMailIcon() {
    final isSelected = selectedChannel == -1;
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 4),
      child: MouseRegion(
        cursor: SystemMouseCursors.click,
        child: GestureDetector(
          onTap: onMailTap,
          child: Container(
            width: 48,
            height: 48,
            decoration: BoxDecoration(
              color: isSelected ? AppColors.primary : const Color(0xFFE8E8E8),
              borderRadius: BorderRadius.circular(isSelected ? 16 : 24),
            ),
            child: Center(
              child: Icon(
                Icons.mail_outline,
                color: isSelected ? Colors.white : AppColors.textSecondary,
                size: 22,
              ),
            ),
          ),
        ),
      ),
    );
  }

  Widget _buildServerIcon({
    required String label,
    required Color color,
    int? index,
    bool isAdd = false,
  }) {
    final isSelected = index != null && selectedChannel == index;
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 4),
      child: MouseRegion(
        cursor: SystemMouseCursors.click,
        child: GestureDetector(
          onTap: isAdd ? () {} : () => onChannelTap(index!),
          child: Container(
            width: 48,
            height: 48,
            decoration: BoxDecoration(
              color: isAdd ? Colors.transparent : color,
              borderRadius: BorderRadius.circular(isSelected ? 16 : 24),
              border: isAdd
                  ? Border.all(color: const Color(0xFF43B581), width: 2)
                  : null,
            ),
            child: isAdd
                ? const Icon(Icons.add, color: Color(0xFF43B581), size: 20)
                : Center(
                    child: Text(
                      label,
                      style: const TextStyle(
                        color: Colors.white,
                        fontSize: 14,
                        fontWeight: FontWeight.bold,
                      ),
                    ),
                  ),
          ),
        ),
      ),
    );
  }
}

class LeftSidebar extends StatelessWidget {
  final MailSubView selectedSubView;
  final Function(MailSubView) onSubViewSelect;

  const LeftSidebar({
    super.key,
    required this.selectedSubView,
    required this.onSubViewSelect,
  });

  @override
  Widget build(BuildContext context) {
    return Container(
      width: 200,
      color: AppColors.surface,
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          // Compose Button
          Padding(
            padding: const EdgeInsets.fromLTRB(16, 24, 16, 20),
            child: SizedBox(
              width: double.infinity,
              child: ElevatedButton.icon(
                onPressed: () {},
                icon: const Icon(Icons.add, size: 16),
                label: const Text('Compose new'),
                style: ElevatedButton.styleFrom(
                  backgroundColor: AppColors.primary,
                  foregroundColor: Colors.white,
                  padding: const EdgeInsets.symmetric(vertical: 14),
                  shape: RoundedRectangleBorder(
                    borderRadius: BorderRadius.circular(8),
                  ),
                ),
              ),
            ),
          ),

          // Messages Section
          const Padding(
            padding: EdgeInsets.symmetric(horizontal: 20),
            child: Text(
              'Messages',
              style: TextStyle(
                fontWeight: FontWeight.w600,
                fontSize: 11,
                color: AppColors.textMuted,
                letterSpacing: 0.5,
              ),
            ),
          ),

          const SizedBox(height: 8),

          // Nav Items
          _buildNavItem(Icons.inbox_outlined, 'Inbox',
              badge: '10',
              isActive: selectedSubView == MailSubView.inbox,
              onTap: () => onSubViewSelect(MailSubView.inbox)),
          _buildNavItem(Icons.send_outlined, 'Sent',
              onTap: () => onSubViewSelect(MailSubView.inbox)),
          _buildNavItem(Icons.edit_outlined, 'Drafts',
              onTap: () => onSubViewSelect(MailSubView.inbox)),
          _buildNavItem(Icons.delete_outline, 'Trash',
              onTap: () => onSubViewSelect(MailSubView.inbox)),
          _buildNavItem(Icons.bookmark_border, 'Saved',
              onTap: () => onSubViewSelect(MailSubView.inbox)),
          _buildNavItem(Icons.folder_outlined, 'Other',
              onTap: () => onSubViewSelect(MailSubView.inbox)),

          const SizedBox(height: 16),

          // Conversations Section
          const Padding(
            padding: EdgeInsets.symmetric(horizontal: 20),
            child: Text(
              'Conversations',
              style: TextStyle(
                fontWeight: FontWeight.w600,
                fontSize: 11,
                color: AppColors.textMuted,
                letterSpacing: 0.5,
              ),
            ),
          ),

          const SizedBox(height: 8),

          _buildNavItem(Icons.chat_bubble_outline, 'Direct Messages',
              isActive: selectedSubView == MailSubView.conversations,
              onTap: () => onSubViewSelect(MailSubView.conversations)),

          const Spacer(),
        ],
      ),
    );
  }

  Widget _buildNavItem(IconData icon, String label, {String? badge, bool isActive = false, VoidCallback? onTap}) {
    return Container(
      margin: const EdgeInsets.symmetric(horizontal: 12, vertical: 2),
      decoration: BoxDecoration(
        color: isActive ? AppColors.selectedNav : Colors.transparent,
        borderRadius: BorderRadius.circular(6),
        border: isActive
            ? const Border(left: BorderSide(color: AppColors.primary, width: 3))
            : null,
      ),
      child: ListTile(
        dense: true,
        contentPadding: const EdgeInsets.symmetric(horizontal: 12),
        leading: Icon(
          icon,
          size: 18,
          color: isActive ? AppColors.primary : AppColors.textSecondary,
        ),
        title: Text(
          label,
          style: TextStyle(
            fontSize: 13,
            color: isActive ? AppColors.primary : AppColors.textPrimary,
            fontWeight: isActive ? FontWeight.w600 : FontWeight.normal,
          ),
        ),
        trailing: badge != null
            ? Container(
                padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 2),
                decoration: BoxDecoration(
                  color: AppColors.primary,
                  borderRadius: BorderRadius.circular(10),
                ),
                child: Text(
                  badge,
                  style: const TextStyle(
                    color: Colors.white,
                    fontSize: 10,
                    fontWeight: FontWeight.bold,
                  ),
                ),
              )
            : null,
        onTap: onTap ?? () {},
      ),
    );
  }
}

class EmailListPane extends StatelessWidget {
  final List<Email> emails;
  final int selectedIndex;
  final Function(int) onSelect;

  const EmailListPane({
    super.key,
    required this.emails,
    required this.selectedIndex,
    required this.onSelect,
  });

  @override
  Widget build(BuildContext context) {
    return Container(
      width: 340,
      color: AppColors.background,
      child: ListView.builder(
        padding: const EdgeInsets.all(2),
        itemCount: emails.length,
        itemBuilder: (context, index) {
          return EmailCard(
            email: emails[index],
            isSelected: index == selectedIndex,
            onTap: () => onSelect(index),
          );
        },
      ),
    );
  }
}

class EmailDetailPane extends StatelessWidget {
  final Email email;

  const EmailDetailPane({super.key, required this.email});

  @override
  Widget build(BuildContext context) {
    return Container(
      color: AppColors.surface,
      child: LayoutBuilder(
        builder: (context, constraints) {
          return SingleChildScrollView(
            padding: const EdgeInsets.all(24),
            child: ConstrainedBox(
              constraints: BoxConstraints(minHeight: constraints.maxHeight),
              child: IntrinsicHeight(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    // Header row: Avatar | Name + Icons
                    Row(
                      crossAxisAlignment: CrossAxisAlignment.center,
                      children: [
                        // Left: Avatar
                        CircleAvatar(
                          radius: 24,
                          backgroundColor: Colors.grey[300],
                          child: Text(
                            email.senderName[0],
                            style: const TextStyle(
                              color: Colors.white,
                              fontWeight: FontWeight.bold,
                              fontSize: 16,
                            ),
                          ),
                        ),
                        const SizedBox(width: 16),
                        // Right: Name + Icons
                        Expanded(
                          child: Row(
                            children: [
                              Expanded(
                                child: Text(
                                  email.senderName,
                                  style: const TextStyle(
                                    fontWeight: FontWeight.w500,
                                    fontSize: 15,
                                    color: AppColors.textPrimary,
                                  ),
                                ),
                              ),
                              // Action icons
                              IconButton(
                                icon: const Icon(Icons.reply, size: 18, color: AppColors.textSecondary),
                                onPressed: () {},
                                padding: EdgeInsets.zero,
                                constraints: const BoxConstraints(),
                              ),
                              const SizedBox(width: 16),
                              IconButton(
                                icon: const Icon(Icons.reply_all, size: 18, color: AppColors.textSecondary),
                                onPressed: () {},
                                padding: EdgeInsets.zero,
                                constraints: const BoxConstraints(),
                              ),
                              const SizedBox(width: 16),
                              IconButton(
                                icon: const Icon(Icons.delete_outline, size: 18, color: AppColors.textSecondary),
                                onPressed: () {},
                                padding: EdgeInsets.zero,
                                constraints: const BoxConstraints(),
                              ),
                            ],
                          ),
                        ),
                      ],
                    ),
                    const SizedBox(height: 20),
                    // Subject row: Bold title | Timestamp
                    Padding(
                      padding: const EdgeInsets.symmetric(horizontal: 64),
                      child: Row(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          Expanded(
                            child: Text(
                              'Hey there! Check out this thing!',
                              style: const TextStyle(
                                fontWeight: FontWeight.bold,
                                fontSize: 22,
                                color: AppColors.textPrimary,
                              ),
                            ),
                          ),
                          Text(
                            'Monday, Apr 7, 2018  10:35',
                            style: TextStyle(
                              fontSize: 11,
                              color: AppColors.textMuted.withValues(alpha: 0.7),
                            ),
                          ),
                        ],
                      ),
                    ),

                    const SizedBox(height: 24),

                    // Body + attachments indented to align with name/subject text
                    Padding(
                      padding: const EdgeInsets.symmetric(horizontal: 64),
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          // Message Body
                          const Text(
                            'Leverage agile frameworks to provide a robust synopsis for high level overviews. Iterative approaches to corporate strategy foster collaborative thinking to further the overall value proposition. Organically grow the holistic world view of disruptive innovation via workplace diversity and empowerment.',
                            style: TextStyle(
                              fontSize: 14,
                              color: AppColors.textSecondary,
                              height: 1.7,
                            ),
                          ),
                          const SizedBox(height: 16),
                          const Text(
                            'Bring to the table win-win survival strategies to ensure proactive domination. At the end of the day, going forward, a new normal that has evolved from generation X is on the runway heading towards a streamlined cloud solution. User generated content in real-time will have multiple touchpoints for offshoring.',
                            style: TextStyle(
                              fontSize: 14,
                              color: AppColors.textSecondary,
                              height: 1.7,
                            ),
                          ),
                          const SizedBox(height: 16),
                          const Text(
                            'Capitalize on low hanging fruit to identify a ballpark value added activity to beta test. Override the digital divide with additional clickthroughs from DevOps.',
                            style: TextStyle(
                              fontSize: 14,
                              color: AppColors.textSecondary,
                              height: 1.7,
                            ),
                          ),

                          const SizedBox(height: 32),

                          // Attachment bar matching image
                          if (email.hasAttachment) ...[
                            Row(
                              children: [
                                // PDF icon box
                                Container(
                                  width: 56,
                                  height: 56,
                                  decoration: BoxDecoration(
                                    color: const Color(0xFFE8E8E8),
                                    borderRadius: BorderRadius.circular(8),
                                  ),
                                  child: const Center(
                                    child: Text(
                                      'PDF',
                                      style: TextStyle(
                                        fontWeight: FontWeight.bold,
                                        fontSize: 14,
                                        color: AppColors.textMuted,
                                      ),
                                    ),
                                  ),
                                ),
                                const SizedBox(width: 12),
                                // Link icon + filename
                                const Icon(Icons.link, size: 16, color: AppColors.textSecondary),
                                const SizedBox(width: 8),
                                Expanded(
                                  child: Text(
                                    'Shop_Presentation_01.pdf (100 KB)',
                                    style: const TextStyle(
                                      fontSize: 13,
                                      color: AppColors.textPrimary,
                                    ),
                                  ),
                                ),
                                // Action buttons
                                TextButton(
                                  onPressed: () {},
                                  child: const Text(
                                    'Share',
                                    style: TextStyle(fontSize: 13, color: AppColors.textSecondary),
                                  ),
                                ),
                                TextButton(
                                  onPressed: () {},
                                  child: const Text(
                                    'Show',
                                    style: TextStyle(fontSize: 13, color: AppColors.textSecondary),
                                  ),
                                ),
                                TextButton(
                                  onPressed: () {},
                                  child: const Text(
                                    'Download',
                                    style: TextStyle(fontSize: 13, color: AppColors.textSecondary),
                                  ),
                                ),
                              ],
                            ),
                          ],
                        ],
                      ),
                    ),
                  ],
                ),
              ),
            ),
          );
        },
      ),
    );
  }
}

class ContactStatusPanel extends StatelessWidget {
  final List<Contact> contacts;

  const ContactStatusPanel({super.key, required this.contacts});

  @override
  Widget build(BuildContext context) {
    final online = contacts.where((c) => c.isOnline).toList();
    final offline = contacts.where((c) => !c.isOnline).toList();

    return Container(
      width: 280,
      decoration: const BoxDecoration(
        color: AppColors.surface,
        border: Border(left: BorderSide(color: AppColors.border)),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          // Header
          Container(
            padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 16),
            decoration: const BoxDecoration(
              border: Border(bottom: BorderSide(color: AppColors.border)),
            ),
            child: const Row(
              mainAxisAlignment: MainAxisAlignment.spaceBetween,
              children: [
                Text(
                  'Contact Status',
                  style: TextStyle(
                    fontWeight: FontWeight.bold,
                    fontSize: 14,
                    color: AppColors.textPrimary,
                  ),
                ),
                Text(
                  'View All',
                  style: TextStyle(fontSize: 12, color: AppColors.primary),
                ),
              ],
            ),
          ),

          // Online Section
          Padding(
            padding: const EdgeInsets.fromLTRB(16, 16, 16, 8),
            child: Text(
              'ONLINE — ${online.length}',
              style: const TextStyle(
                fontSize: 11,
                fontWeight: FontWeight.w700,
                color: AppColors.textMuted,
                letterSpacing: 0.5,
              ),
            ),
          ),
          ...online.map((c) => _buildContactRow(c)),

          // Offline Section
          Padding(
            padding: const EdgeInsets.fromLTRB(16, 16, 16, 8),
            child: Text(
              'OFFLINE — ${offline.length}',
              style: const TextStyle(
                fontSize: 11,
                fontWeight: FontWeight.w700,
                color: AppColors.textMuted,
                letterSpacing: 0.5,
              ),
            ),
          ),
          ...offline.map((c) => _buildContactRow(c)),
        ],
      ),
    );
  }

  Widget _buildContactRow(Contact contact) {
    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 6),
      child: Row(
        children: [
          Stack(
            children: [
              CircleAvatar(
                radius: 16,
                backgroundColor: (contact.avatarColor ?? Colors.grey).withValues(alpha: 0.2),
                child: Text(
                  contact.name[0],
                  style: TextStyle(
                    color: contact.avatarColor ?? Colors.grey,
                    fontSize: 12,
                    fontWeight: FontWeight.bold,
                  ),
                ),
              ),
              if (contact.isOnline)
                Positioned(
                  bottom: 0,
                  right: 0,
                  child: Container(
                    width: 10,
                    height: 10,
                    decoration: BoxDecoration(
                      color: contact.isPlaying ? const Color(0xFF43B581) : Colors.green,
                      shape: BoxShape.circle,
                      border: Border.all(color: AppColors.surface, width: 2),
                    ),
                  ),
                ),
            ],
          ),
          const SizedBox(width: 10),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  contact.name,
                  style: TextStyle(
                    fontSize: 13,
                    fontWeight: FontWeight.w600,
                    color: contact.isOnline ? AppColors.textPrimary : AppColors.textMuted,
                  ),
                ),
                if (contact.status != null)
                  Text(
                    contact.status!,
                    style: const TextStyle(
                      fontSize: 11,
                      color: AppColors.textMuted,
                    ),
                    overflow: TextOverflow.ellipsis,
                  ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}

class ContactListPane extends StatelessWidget {
  final List<Contact> contacts;
  final int selectedIndex;
  final Function(int) onSelect;

  const ContactListPane({
    super.key,
    required this.contacts,
    required this.selectedIndex,
    required this.onSelect,
  });

  @override
  Widget build(BuildContext context) {
    return Container(
      width: 340,
      color: const Color(0xFFEAEEF4),
      child: ListView.builder(
        padding: const EdgeInsets.all(8),
        itemCount: contacts.length,
        itemBuilder: (context, index) {
          final contact = contacts[index];
          final isSelected = index == selectedIndex;
          return InkWell(
            onTap: () => onSelect(index),
            borderRadius: BorderRadius.circular(8),
            child: Container(
              margin: const EdgeInsets.symmetric(vertical: 2),
              padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 10),
              decoration: BoxDecoration(
                color: isSelected ? AppColors.selectedNav : Colors.transparent,
                borderRadius: BorderRadius.circular(8),
                border: isSelected
                    ? const Border(left: BorderSide(color: AppColors.primary, width: 3))
                    : null,
              ),
              child: Row(
                children: [
                  Stack(
                    children: [
                      CircleAvatar(
                        radius: 18,
                        backgroundColor: (contact.avatarColor ?? Colors.grey).withValues(alpha: 0.2),
                        child: Text(
                          contact.name[0],
                          style: TextStyle(
                            color: contact.avatarColor ?? Colors.grey,
                            fontSize: 12,
                            fontWeight: FontWeight.bold,
                          ),
                        ),
                      ),
                      if (contact.isOnline)
                        Positioned(
                          bottom: 0,
                          right: 0,
                          child: Container(
                            width: 10,
                            height: 10,
                            decoration: BoxDecoration(
                              color: contact.isPlaying ? const Color(0xFF43B581) : Colors.green,
                              shape: BoxShape.circle,
                              border: Border.all(color: AppColors.background, width: 2),
                            ),
                          ),
                        ),
                    ],
                  ),
                  const SizedBox(width: 12),
                  Expanded(
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        Text(
                          contact.name,
                          style: TextStyle(
                            fontSize: 14,
                            fontWeight: FontWeight.w600,
                            color: isSelected ? AppColors.primary : AppColors.textPrimary,
                          ),
                        ),
                        if (contact.status != null)
                          Text(
                            contact.status!,
                            style: const TextStyle(
                              fontSize: 12,
                              color: AppColors.textMuted,
                            ),
                            overflow: TextOverflow.ellipsis,
                          ),
                      ],
                    ),
                  ),
                ],
              ),
            ),
          );
        },
      ),
    );
  }
}

class SingleContactPanel extends StatelessWidget {
  final Contact contact;

  const SingleContactPanel({super.key, required this.contact});

  @override
  Widget build(BuildContext context) {
    return Container(
      width: 280,
      decoration: const BoxDecoration(
        color: Color(0xFFEAEEF2),
        border: Border(left: BorderSide(color: AppColors.border)),
      ),
      child: LayoutBuilder(
        builder: (context, constraints) {
          return SingleChildScrollView(
            child: Container(
              constraints: BoxConstraints(minHeight: constraints.maxHeight),
              padding: const EdgeInsets.all(12),
              alignment: Alignment.topCenter,
              decoration: const BoxDecoration(
                color: Color(0xFFEAEEF2),
              ),
              child: Container(
                clipBehavior: Clip.antiAlias,
                decoration: BoxDecoration(
                  color: AppColors.surface,
                  borderRadius: BorderRadius.circular(12),
                ),
                child: Column(
                  mainAxisSize: MainAxisSize.min,
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    // Banner
                    Container(
                      height: 100,
                      color: (contact.avatarColor ?? AppColors.primary).withValues(alpha: 0.3),
                    ),

                    // Avatar overlapping banner
                    Padding(
                      padding: const EdgeInsets.symmetric(horizontal: 16),
                      child: Transform.translate(
                        offset: const Offset(0, -30),
                        child: CircleAvatar(
                          radius: 36,
                          backgroundColor: AppColors.surface,
                          child: CircleAvatar(
                            radius: 32,
                            backgroundColor: (contact.avatarColor ?? Colors.grey).withValues(alpha: 0.2),
                            child: Text(
                              contact.name[0],
                              style: TextStyle(
                                color: contact.avatarColor ?? Colors.grey,
                                fontSize: 24,
                                fontWeight: FontWeight.bold,
                              ),
                            ),
                          ),
                        ),
                      ),
                    ),

                    // Name and handle
                    Padding(
                      padding: const EdgeInsets.fromLTRB(16, 0, 16, 8),
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          Text(
                            contact.name,
                            style: const TextStyle(
                              fontWeight: FontWeight.bold,
                              fontSize: 18,
                              color: AppColors.textPrimary,
                            ),
                          ),
                          const SizedBox(height: 2),
                          Text(
                            contact.handle,
                            style: const TextStyle(
                              fontSize: 13,
                              color: AppColors.textSecondary,
                            ),
                          ),
                          if (contact.status != null) ...[
                            const SizedBox(height: 8),
                            Container(
                              padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
                              decoration: BoxDecoration(
                                color: AppColors.background,
                                borderRadius: BorderRadius.circular(16),
                              ),
                              child: Text(
                                contact.status!,
                                style: const TextStyle(
                                  fontSize: 12,
                                  color: AppColors.textSecondary,
                                ),
                              ),
                            ),
                          ],
                        ],
                      ),
                    ),

                    const Divider(height: 1),

                    // Member Since
                    if (contact.memberSince != null)
                      Padding(
                        padding: const EdgeInsets.all(16),
                        child: Column(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: [
                            const Text(
                              'Member Since',
                              style: TextStyle(
                                fontSize: 11,
                                fontWeight: FontWeight.w700,
                                color: AppColors.textMuted,
                                letterSpacing: 0.5,
                              ),
                            ),
                            const SizedBox(height: 4),
                            Text(
                              contact.memberSince!,
                              style: const TextStyle(
                                fontSize: 13,
                                color: AppColors.textPrimary,
                              ),
                            ),
                          ],
                        ),
                      ),

                    // Mutual Servers
                    if (contact.mutualServers > 0)
                      Padding(
                        padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
                        child: Row(
                          mainAxisAlignment: MainAxisAlignment.spaceBetween,
                          children: [
                            Text(
                              'Mutual Servers — ${contact.mutualServers}',
                              style: const TextStyle(
                                fontSize: 13,
                                fontWeight: FontWeight.w600,
                                color: AppColors.textPrimary,
                              ),
                            ),
                            const Icon(Icons.keyboard_arrow_right, size: 18, color: AppColors.textMuted),
                          ],
                        ),
                      ),

                    // Mutual Friends
                    if (contact.mutualFriends > 0)
                      Padding(
                        padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
                        child: Row(
                          mainAxisAlignment: MainAxisAlignment.spaceBetween,
                          children: [
                            Text(
                              'Mutual Friends — ${contact.mutualFriends}',
                              style: const TextStyle(
                                fontSize: 13,
                                fontWeight: FontWeight.w600,
                                color: AppColors.textPrimary,
                              ),
                            ),
                            const Icon(Icons.keyboard_arrow_right, size: 18, color: AppColors.textMuted),
                          ],
                        ),
                      ),
                  ],
                ),
              ),
            ),
          );
        },
      ),
    );
  }
}

// ==================== CHAT VIEW WIDGETS ====================

class TopicList extends StatelessWidget {
  final String channelName;
  final List<Topic> topics;
  final int selectedIndex;
  final Function(int) onSelect;

  const TopicList({
    super.key,
    required this.channelName,
    required this.topics,
    required this.selectedIndex,
    required this.onSelect,
  });

  @override
  Widget build(BuildContext context) {
    return Container(
      width: 200,
      color: const Color(0xFFEAEEF4),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          // Server Header
          Container(
            padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 18),
            decoration: const BoxDecoration(
              border: Border(bottom: BorderSide(color: AppColors.border)),
            ),
            child: Row(
              children: [
                Expanded(
                  child: Text(
                    channelName,
                    style: const TextStyle(
                      fontWeight: FontWeight.bold,
                      fontSize: 14,
                      color: AppColors.textPrimary,
                    ),
                    overflow: TextOverflow.ellipsis,
                  ),
                ),
                const Icon(Icons.keyboard_arrow_down, size: 18, color: AppColors.textSecondary),
              ],
            ),
          ),

          // Channels
          Expanded(
            child: ListView.builder(
              padding: const EdgeInsets.symmetric(vertical: 8),
              itemCount: topics.length + 1, // +1 for a category header
              itemBuilder: (context, index) {
                if (index == 0) {
                  return const Padding(
                    padding: EdgeInsets.fromLTRB(16, 8, 16, 4),
                    child: Text(
                      'TEXT TOPICS',
                      style: TextStyle(
                        fontSize: 10,
                        fontWeight: FontWeight.w700,
                        color: AppColors.textMuted,
                        letterSpacing: 0.8,
                      ),
                    ),
                  );
                }
                final topic = topics[index - 1];
                final isActive = index - 1 == selectedIndex;
                return _buildTopicItem(topic, isActive, () => onSelect(index - 1));
              },
            ),
          ),
        ],
      ),
    );
  }

  Widget _buildTopicItem(Topic topic, bool isActive, VoidCallback onTap) {
    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 1),
      child: InkWell(
        onTap: onTap,
        borderRadius: BorderRadius.circular(4),
        child: Container(
          padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
          decoration: BoxDecoration(
            color: isActive ? AppColors.selectedNav : Colors.transparent,
            borderRadius: BorderRadius.circular(4),
          ),
          child: Row(
            children: [
              Icon(
                Icons.tag,
                size: 16,
                color: isActive ? AppColors.primary : AppColors.textMuted,
              ),
              const SizedBox(width: 6),
              Expanded(
                child: Text(
                  topic.name,
                  style: TextStyle(
                    fontSize: 14,
                    fontWeight: isActive ? FontWeight.w600 : FontWeight.w500,
                    color: isActive ? AppColors.primary : AppColors.textSecondary,
                  ),
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}

class ChatArea extends StatelessWidget {
  final String topicName;

  const ChatArea({
    super.key,
    required this.topicName,
  });

  @override
  Widget build(BuildContext context) {
    return Container(
      color: AppColors.surface,
      child: Column(
        children: [
          // Chat messages
          Expanded(
            child: ListView(
              padding: const EdgeInsets.all(16),
              children: [
                _buildMessage(
                  avatarColor: Colors.purple[100]!,
                  avatarText: 'C',
                  textColor: Colors.purple,
                  name: 'Calo',
                  time: '5:03 PM',
                  message: 'Check out this new design concept I just finished!',
                  isSent: false,
                ),
                const SizedBox(height: 16),
                _buildMessage(
                  avatarColor: Colors.orange,
                  avatarText: 'M',
                  textColor: Colors.white,
                  name: 'Michael',
                  time: '5:12 PM',
                  message: 'Looks great! Can you share the Figma link?',
                  isSent: true,
                ),
                const SizedBox(height: 16),
                _buildMessage(
                  avatarColor: Colors.green[100]!,
                  avatarText: 'I',
                  textColor: Colors.green,
                  name: 'iceberg_ssj',
                  time: '5:30 PM',
                  message: 'Who would do that?',
                  isSent: false,
                ),
                const SizedBox(height: 16),
                _buildMessage(
                  avatarColor: Colors.blue[100]!,
                  avatarText: 'S',
                  textColor: Colors.blue,
                  name: 'Superman',
                  time: '6:41 PM',
                  message: 'https://www.instagram.com/reel/DU_S98hkZvz/',
                  isSent: false,
                  isLink: true,
                ),
                const SizedBox(height: 16),
                _buildMessage(
                  avatarColor: Colors.blue[100]!,
                  avatarText: 'S',
                  textColor: Colors.blue,
                  name: 'Superman',
                  time: '7:18 PM',
                  message: 'https://www.instagram.com/p/DXU6RxUDJWb/',
                  isSent: false,
                  isLink: true,
                ),
                const SizedBox(height: 16),
                _buildMessage(
                  avatarColor: Colors.orange,
                  avatarText: 'M',
                  textColor: Colors.white,
                  name: 'Michael',
                  time: '7:45 PM',
                  message: 'Nice find! Let me check these out.',
                  isSent: true,
                ),
              ],
            ),
          ),

          // Message Input
          Container(
            padding: const EdgeInsets.all(16),
            decoration: const BoxDecoration(
              color: Color(0xFFC9E0EC),
              border: Border(top: BorderSide(color: AppColors.border)),
            ),
            child: Row(
              children: [
                Icon(Icons.add_circle_outline, size: 24, color: AppColors.textMuted.withValues(alpha: 0.6)),
                const SizedBox(width: 12),
                Expanded(
                  child: Container(
                    padding: const EdgeInsets.symmetric(horizontal: 16),
                    decoration: BoxDecoration(
                      color: AppColors.background,
                      borderRadius: BorderRadius.circular(24),
                    ),
                    child: const TextField(
                      decoration: InputDecoration(
                        hintText: 'Message #topic',
                        hintStyle: TextStyle(fontSize: 14, color: AppColors.textMuted),
                        border: InputBorder.none,
                        isDense: true,
                        contentPadding: EdgeInsets.symmetric(vertical: 12),
                      ),
                    ),
                  ),
                ),
                const SizedBox(width: 12),
                Icon(Icons.emoji_emotions_outlined, size: 22, color: AppColors.textMuted.withValues(alpha: 0.6)),
              ],
            ),
          ),
        ],
      ),
    );
  }

  Widget _buildMessage({
    required Color avatarColor,
    required String avatarText,
    required Color textColor,
    required String name,
    required String time,
    required String message,
    required bool isSent,
    bool isLink = false,
  }) {
    if (isSent) {
      return Row(
        mainAxisAlignment: MainAxisAlignment.end,
        children: [
          Flexible(
            child: Container(
              padding: const EdgeInsets.all(12),
              decoration: BoxDecoration(
                color: AppColors.primary,
                borderRadius: BorderRadius.circular(12),
              ),
              child: Text(
                message,
                style: const TextStyle(
                  fontSize: 14,
                  color: Colors.white,
                  height: 1.4,
                ),
              ),
            ),
          ),
        ],
      );
    }

    return Row(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        CircleAvatar(
          radius: 18,
          backgroundColor: avatarColor,
          child: Text(
            avatarText,
            style: TextStyle(
              color: textColor,
              fontSize: 12,
              fontWeight: FontWeight.bold,
            ),
          ),
        ),
        const SizedBox(width: 12),
        Expanded(
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Row(
                children: [
                  Text(
                    name,
                    style: const TextStyle(
                      fontSize: 13,
                      fontWeight: FontWeight.w600,
                      color: AppColors.textPrimary,
                    ),
                  ),
                  const SizedBox(width: 8),
                  Text(
                    time,
                    style: const TextStyle(
                      fontSize: 11,
                      color: AppColors.textMuted,
                    ),
                  ),
                ],
              ),
              const SizedBox(height: 4),
              Text(
                message,
                style: TextStyle(
                  fontSize: 14,
                  color: isLink ? AppColors.primary : AppColors.textSecondary,
                  height: 1.4,
                ),
              ),
            ],
          ),
        ),
      ],
    );
  }
}

class MembersPanel extends StatelessWidget {
  const MembersPanel({super.key});

  @override
  Widget build(BuildContext context) {
    return Container(
      width: 280,
      decoration: const BoxDecoration(
        color: AppColors.surface,
        border: Border(left: BorderSide(color: AppColors.border)),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          // Online Section
          const Padding(
            padding: EdgeInsets.fromLTRB(16, 16, 16, 8),
            child: Text(
              'ONLINE — 4',
              style: TextStyle(
                fontSize: 11,
                fontWeight: FontWeight.w700,
                color: AppColors.textMuted,
                letterSpacing: 0.5,
              ),
            ),
          ),
          _buildMember('Calo', Colors.purple, status: 'Playing League of Legends', isPlaying: true),
          _buildMember('iceberg_ssj', Colors.green),
          _buildMember('Superman', Colors.blue),
          _buildMember('Nicole Adams', Colors.pink, status: 'Working on designs'),

          // Offline Section
          const Padding(
            padding: EdgeInsets.fromLTRB(16, 16, 16, 8),
            child: Text(
              'OFFLINE — 3',
              style: TextStyle(
                fontSize: 11,
                fontWeight: FontWeight.w700,
                color: AppColors.textMuted,
                letterSpacing: 0.5,
              ),
            ),
          ),
          _buildMember('Lily Jones', Colors.grey, isOnline: false),
          _buildMember('Marie Clark', Colors.grey, isOnline: false),
          _buildMember('Mark Conners', Colors.grey, isOnline: false),
        ],
      ),
    );
  }

  Widget _buildMember(String name, Color color, {String? status, bool isOnline = true, bool isPlaying = false}) {
    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 6),
      child: Row(
        children: [
          Stack(
            children: [
              CircleAvatar(
                radius: 16,
                backgroundColor: color.withValues(alpha: 0.2),
                child: Text(
                  name[0],
                  style: TextStyle(
                    color: color,
                    fontSize: 12,
                    fontWeight: FontWeight.bold,
                  ),
                ),
              ),
              if (isOnline)
                Positioned(
                  bottom: 0,
                  right: 0,
                  child: Container(
                    width: 10,
                    height: 10,
                    decoration: BoxDecoration(
                      color: isPlaying ? const Color(0xFF43B581) : Colors.green,
                      shape: BoxShape.circle,
                      border: Border.all(color: AppColors.surface, width: 2),
                    ),
                  ),
                ),
            ],
          ),
          const SizedBox(width: 10),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  name,
                  style: TextStyle(
                    fontSize: 13,
                    fontWeight: FontWeight.w600,
                    color: isOnline ? AppColors.textPrimary : AppColors.textMuted,
                  ),
                ),
                if (status != null)
                  Text(
                    status,
                    style: const TextStyle(
                      fontSize: 11,
                      color: AppColors.textMuted,
                    ),
                    overflow: TextOverflow.ellipsis,
                  ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}
