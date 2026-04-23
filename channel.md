We need to work on the concept of channels within the system. Channels will be composed of a meridian network and the
Quasar overlay together. The default channel which will load when the system starts up is known as the dial channel. The
channel will allow users to find other nodes and to create additional channels.
When one creates a channel for the first time, they generate the PKI information representing that channel. The Base58
hash of the public key represents the topic of that channel, on the dial channel. Additionally, along with the PKI
information, the user will create the port range in which the channel's network will operate. If a new node wants to
join the channel from the dial channel, they publish a bootstrap message to the topic for the channel on the dial
channel. that are listening to the topic for the channel will respond to the nodes directly using the Dow channel giving
it connection information on how to access the channel's network.
As a node is on a channel's network, it will have the same Quasar and Meridian capabilities that one would normally have
available.

Further need to extend Quasar's functionality to add the ability to publish and subscribe to subtopics. Topics will just
be forwardslash delimited topics that only client applications to the daemon will subscribe to alone.
We must create features for client applications. As per the kikstart.md, applications consume this service over various
network protocols. Create a directory in the source folder called ClientAPIs. In this folder we will start          
implementing the various client APIs for the various network protocols mentioned in the Kickstart.md. Clients need the
ability to create, join, and destroy channels. Within channels they need the ablity to subscribe to topics, unsubscribe
to topics. Topics need to be unique. Perhaps they should have GUIDs when they're created and or base58 hashes of GUIDs
to identify themselves. Really the deciding factor is shorter or what is easier for humans to read and communicate. Al
also alias them with a human readable name defined locally. So you could `Subcribe("Alice/Feeds/friend-only")` or
`Subcribe("X4jKL/Feeds/friend-only")`
where the first part of the path is always the topic or the alias to the topic and the rest of it is the subtopic. If
the alias is ambiguous it returns an error. The deamon would always listen on the subtopic but only transmit to client base upon
the granularity they have subscribed to.