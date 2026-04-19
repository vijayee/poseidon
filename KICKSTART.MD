Alright, so we're kicking off a new project called Poseidon. Poseidon is a peer to peer pub sub network. You can think
of it like IRC, but instead of person to person chat, it is for data between applications. This will allow for the
establishment of ad hoc peer to peer networks. We'll be varying several peer to peer techniques in order to build this
network. We'll be using the Meridian peer to peer network as the basis for network structure. In the references folder
in this directory we have the Meridian research paper and meridian.pdf and also a sample implementation called
libmeridian. For the pub sub functionality we're relying on what is known as a quasar network. In the references folder
there is a quasar.pdf file that shows the research paper that talks about the implementation of quasar. Quasar will act
like an overlay network to Meridian. Additionally, there's a paper in the references folder called self organizing p2p
social networks. It outlines a series of controls that are used in a peer to peer network to organize
based upon the quality of connection and relationship with other peers in a network. We will try to incorporate where
possible this type of method methodology. This will mean our requests are organized into three phases rather than the
normal two. There will be requests, there will be response, and there will be review. The review phase will help nodes
evaluate the quality of their connection to other peers and the quality of those peers participation within the
network. If the rating of a peer gets low enough, a note may choose to disconnect from that peer as it may be malicious
or malfunctioning.

This project will be a C make project and will be built in C. We will be using Google test for testing. We will be using
libc bore for encoding messages that we send over the wire for this protocol. We will be using MSQUIC as the means of
connection between our nodes. This is an implementation of the QUIC protocol. There's also a library called poll-dancer,
which we can make use of for event driven IO for the network. I've also added xxhash and a hash map library for use when
it applies. All of these are located in the depths folder as Git Sub modules.

In the references folder, libMeridian will be our guide for how to implement the Meridian protocol in C, but we will be
modifying it so that it both uses the quick protocol and also has Nat traversal. We want to make use of whatever natural
algorithm makes the most sense in the network environment of the node. You may have to figure this out automatically.
For insight on and guidance on how to do this, I have included in the references folder iroh. Within it should be
applicable nat traversal code we can copy. You may also
reference https://github.com/libp2p/go-libp2p/tree/master/p2p/protocol/holepunch for ideas on nat traversal.

Nodes in the network will be identified by PKI. Identifier of the note will be the base58 hash of its public key. We
may generate this with OpenSSL as well as a full certificate that can be used by the QUIC protocol for connecting. The
network communication will be divided into channels. Each channel will have its own port or series of ports that can be
used to connect to it. Users will automatically be connected to the dial channel, which will be the default channel when
the network starts. The dial channel will be used to find specific peer nodes within the network.
It will also be used to bootstrap nodes to other channels. The channel can be created at any time and will be uniquely
identified by the base58 hash of the generated public key. Every channel will consist of a Meridian network with a
Quasar overlay. It would also have unique ports or series of ports to connect to. To connect to a channel for the first
time you will
either bootstrap to a known node that's already on the channel's network through the dial channel or reconnect to peers
that are known to be connected to that channel.

The node itself will be a daemon that runs on the computer. Client applications will connect to it over Unix sockets,
TCP, WebSockets, or QUIC. Nodes will act as a broker between the client applications and the channels themselves.

We will start with trying to implement and marry together quasar and meridian. The src directory is pre-filled
with libraries and utilities and code that I thought would be helpful from other code bases. Get familiar with it.
