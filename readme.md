# What is this?

Morpheus is a **Work in progress** IRC gateway to a [Matrix](https://matrix.org) server.
It translates between the two protocols so that you can view Matrix rooms via
any IRC client.

# Is it a bridge like appservice-irc?

Matrix's appservice-irc bridges rooms on an existing Matrix server with rooms on
an existing IRC server. With morpheus *there is no IRC server*.
In other words, morpheus acts as an IRCd itself, providing a view onto a Matrix
server through the IRC protocol

# Why would I want this?

There are two main reasons:

1) You have an IRC bot that you want to have on a Matrix server without changing
   any of its code.
2) You are connected to other IRC networks in your favourite IRC client, and
   would rather use that than a separate program to talk on Matrix.

# What works currently?

* Multiple clients
* Everything going via epoll
* Logging in via PASS + USER
* Joining / Parting rooms
* Messages to channels
	* canonical alias is used as the IRC channel name
* Private messages to users
	* transparently creates / re-uses 1:1 private rooms
	* works with query buffers in IRC clients
* Message formatting
	* converts between IRC control codes and org.matrix.custom.html format
* Setting room topics
* Converting inline media to links
* IRCv3 Capabilities
	* server-time for old messages
	* away-notify for presence updates (not fully implemented)
* Other stuff I'm probably forgetting

# What needs to be done?

* Kicking / banning
* Giving unique nicks to users from other homeservers
* TLS support on the IRC socket via mbedTLS
* Other stuff, see TODO.txt

# Other caveats?

* It's written in C and has not been thoroughly tested or cleaned up (yet),
so there are probably horrendous memory corrupting, world-destroying bugs
lurking within. Use at your own risk!
* GNU/Linux only currently.

# Usage

Edit start.sh, setting `MTX_SERVER`, `MTX_NAME` and maybe `MTX_LISTEN_PORT`.
`MTX_NAME` should probably be just the domain of `MTX_SERVER`.
`MTX_LISTEN_PORT` is where the "IRCd" side of morpheus will listen.

Next, add a server to your IRC client. Assuming you're running morpheus on your local
computer, you'd add a server of 127.0.0.1/PORT where PORT is what you set `MTX_LISTEN_PORT`
to, or 1999 by default. Set the username and server password to your matrix userid and password.

If all goes well, you should be connected to the matrix server when you connect to morpheus.
