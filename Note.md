# Spanner Replication codebase

## Transport/TCP/Event Loop
- Transport/TCP/Event Loop
	- TCPTransport::TCPTransport constructor sets up libevent!!
		- using <mark style="background: #ADCCFFA6;">event_base_new_with_config</mark>
		- all the Timer \* stuff is also implemented in `lib/transport.cc` and `lib/tcptransport.cc`
	- Register calls TCPAcceptCallback in `lib/tcptransport.cc`
	- ConnectTCP **and** TCPAcceptCallback set TCPReadableCallback as the readcb in `lib/tcptransport.cc` as well as TCPOutgoingEventCallback **and** TCPIncomingEventCallback, respectively
		- ---> they do so using <mark style="background: #ADCCFFA6;">bufferevent_setcb</mark>
			- bufferevent_setcb(bufev, readcb, writecb, eventcb, cbarg)
			- <mark style="background: #ADCCFFA6;">event_new</mark> allocates and constructs a new event structure with the cb that is invoked when the event occurs
			- <mark style="background: #ADCCFFA6;">event_add</mark> adds it to the event loop
		- Connect is for the direction of me sending to you, Accept is me receiving a connect from you so that you can send to me
	- **whenever a message is sent**, it's done using SendMessageToReplica in `lib/transportcommon.h`
		- ---> which calls SendMessageInternal in `lib/tcptransport.cc` (cuz we're using tcp)
			- ---> which calls <mark style="background: #ADCCFFA6;">bufferevent_write</mark>, this is in `<event.h>`the libevent library for C. 
			- "The **event** API provides a mechanism to execute a function when a specific event on a file descriptor occurs or after a given time has passed."
			- tcpOutgoing is the map of \<receiving address --> events\> `lib/tcptransport.h`
				- sendMessageInternal uses this one
			- tcpAddresses is the map of \<events-->receiving address\>
	- **all incoming traffic gets passed to** TCPReadableCallback in `lib/tcptransport.cc`
		- tcpAddresses is read from
		- ---> which then calls ReceiveMessage in `vr/replica.cc`
## VR Replication Protocol
- VR Replication
	- ReceiveMessage is the only publically exposed function in `replication/vr/replica.h`
		- giant select statement
		- it just cases on the message type in a giant select (request, prepare, prepareOK, commit, requestStateTransfer, stateTransfer, startViewChange, doViewChange, startView) and calls the proper handler for that
	-  HandleRequest in `replication/vr/replica.cc`
		- if not leader --> ignore
		- if leader --> LeaderUpcall
			- ---> calls LeaderUpcall in `replication/common/replica.cc`
				- ---> calls app->LeaderUpcall
	- Commit and reply to client in `replication/vr/replica.cc`
		- Currently Commit to quorum is NOT piggybacked on the next PREPARE
		- SendMessage(client, reply) from `lib/transportcommon.h`
			- ---> which calls SendMessageInternal with replication.vr.proto.ReplyMessage
## VR Client library
- VR Client library
	- all inside of `replication/vr/client.cc`
	- Invoke
		- ---> calls SendRequest
			- ---> calls SendMessageToGroups in `lib/transportcommon.h`
				- ---> invokes TCPReadableCallback with proto::RequestMessage reqMsg which goes to handler
	- all incoming traffic gets passed to TCPReadableCallback in `lib/tcptransport.cc`
		- ---> invokes ReceiveMessage
			- ---> invokes HandleReply

## Spanner Txn Protocol
- Server constructor in `src/store/strongstore/server.cc`
	- ---> replica_client_ = ReplicaClient constructor
		- ---> client = VRClient constructor
- ReceiveMessage is the only publically exposed function in `store/strongstore/server.h`
	- giant select statement
	- t just cases on the message type in a giant select (Get, RWCommitCoordinator, RWCommitParticipant, PrepareOK, PrepareAbort, ROCommit, Abort, Wound, PingMessage) and calls the proper handler for that
- HandleRWCommitParticipant in `store/strongstore/server.cc`
	- ---> invokes replica_client_->Prepare in `store/strongstore/replicaclient.cc`
		- ---> invokes client-->Invoke in `replication/vr/client.cc`
	- *not seeing this function in the logs rn!*
- HandlePrepareOK
	- --->invokes replica_client_->CoordinatorCommit
		- ---> invokes client->Invoke
- QUESTION: where does the receiving of stuff happen?

## Experiments
- all inside of `src/store/server.cc` main()
	- ---> invokes new TCPTransport constructor
	- ---> invokes new Server constructor
	- ---> invokes new VRReplica constructor
- test runs at tport->Run(); from `lib/tcptransport.cc`
	- ---> invokes <mark style="background: #ADCCFFA6;">event_base_dispatch</mark> which "starts the event loop"
		- It runs an event loop that waits for events to occur and then dispatches them to their corresponding callback functions. 
		- The function blocks until there are no more registered events or until <mark style="background: #ADCCFFA6;">event_base_loopbreak</mark> or `event_base_loopexit()` is called.
- Test stops by binding Cleanup function to SIGTERM (etc) signals 
	- ---> invokes  tport->Stop
		- ---> invokes <mark style="background: #ADCCFFA6;">event_base_loopbreak</mark>
- Client starts at `store/benchmark/async/benchmark.cc`
	- all the flags that get passed in from the command line as formatted in `experiments/lib/rss_codebase.py` can be found in this file as FLAGS_xxx. cuz gflags parses them and reads them in and stores them that way.
	- bench_mode = OPEN or CLOSED
		- <mark style="background: #FFB86CA6;">const bench_args = {"open", "closed"}</mark>
	- BenchmarkClientMode 
		- <mark style="background: #FFB86CA6;">const bench_modes = {OPEN, CLOSED}</mark>
	- benchmark = "retwis" or "tpcc"
		- <mark style="background: #FFB86CA6;">const benchmark_args = {"retwis"}</mark>
	- Benchmodes
		- <mark style="background: #FFB86CA6;">const benchmode_t benchmodes[]{BENCH_RETWIS};</mark>
	- creates a vector of Client \* (clients) and BenchmarkClient \* (benchClients)
		- the clients are strongstore::Clients found in `src/store/strongstore/client.cc`
			- starts a client for each shard
			- *these* clients are passed into the retwis::RetwisClient (bench)!!
	- defines bench_done_callback bdcb = \[\&a, \&b, \&c\](){...code...}
			- a, b, c are variables "captured by reference", so the lambda can modify these
			- () means it takes in no parameters
			- {} is the body
				- ensures only first client to finish writes out the stats
				- sums across benchClients!!! (NOT clients)
	- bench = new retwis::RetwisClient{...}
		- takes in Client \* (clients), the shard clients
	- tport->Timer(0, \[bench, bdcb\]\(\){ bench->Start(bdcb); });
		- Timer(uint64_t ms, timer_callback_t cb)
			- which is also typedef std::function<void()> timer_callback_t;
			- this function just moves the callback (via std::move(cb)) to an "info" struct where it's **added to the eventloop**
		- in the retwis file, Start() is the starting point!!
			- which inherits from bench_client!
	- benchClients.push_back(bench);
		- this file is run for each client process... so the vector list has as many processes
		- need to pushback here for bdcb so it can sum up stats across all of them
	- tport->Run();
		- **starts event loop**
		- starts transport layer, gets client connections up and running

## Spanner Client (transaction client from Experiments)
- Client inside of `src/store/benchmark/async/bench_client.cc`
	- lambdas
		- `bench_client.h` defines a bench_done_callback type and execute_callback type, *both* std::function\<void()\> types (lambdas)
			- std::function\<void()\> means it returns void but takes in nothing?
	- **everywhere** you see TimerMicro, this places a function on the eventloop!!
	- entrypoint in Start(bdcb)
		- stores bdcb in curr_bdcb_ which is a *private* member (`bench_client.h`) so that the Cleanup method can reference it, which itself is called when the client is terminated by a signal
		- ---> invokes SendNext
			- ---> invokes client.Begin from `strongstore/client.cc` which starts a transaction thru some RSS stuff
			- uses std::bind(&Client::ContinueBegin, this, std::ref(session), bcb)
			- and auto bcb = std::bind(&BenchmarkClient::ExecuteNextOperation, this, sid);
			- calls Begin on shard client and then bcb()
				- ---> invokes <mark style="background: #BBFABBA6;">ExecuteNextOperation</mark>
				- calls GetNextOperation which is a RETWIS thing (workload specific)
					- ---> invokes client.Get from `strongstore/client.cc`
						- ---> invokes sclients_[i]->Get from `strongstore/shardclient.cc`
							- ---> SendMessageToReplica which becomes SendMessageInternal... this applies the get and...
								- ---> ReceiveMessage from `strongstore/shardclient.cc`
								- HandleGetReply
									- ---> invokes GetCallback (gcb())! which just calls <mark style="background: #BBFABBA6;">ExecuteNextOperation</mark>!!
				- when it calls Commit
					- ---> invokes client.Commit from `strongstore/client.cc`
					- loops thru all the shards involved in this transaction and 
						- ---> invokes shardclient->RWCommitCoordinator to coordinator in `strongstore/shardclient.cc`
						- ---> invokes shardclient->RWCommitParticipant ""
- OnReply records the latency!!!
	- print GetTransactionType which prints out ttype_ which is instantiated when the transaction is initialized as a RetwisTransaction type
- `strongstore/client.cc`
	- session management
- only thing special about retwis client is it specifically defines GetNextTransaction!
- QUESTION: where is server.cc added as the binary? and same for benchclient?
	- see the makefile


# C++ Specifics
## Event vs. Bufferevent
### Event
An event represents a notification that something has happened, such as a file descriptor becoming ready for reading or writing, a timeout expiring, or a signal being received. It is a low-level mechanism that requires the user to manage the actual reading and writing of data.

### Bufferevent
A bufferevent, on the other hand, is a higher-level abstraction that builds upon event. It provides input and output buffers, and automatically handles the reading and writing of data to and from the underlying transport (e.g., a socket). The user interacts with the bufferevent by adding data to the output buffer and reading data from the input buffer, without needing to directly manage the I/O operations.

### When to use which
Use event when you need fine-grained control over I/O operations or when you are dealing with non-data-stream events like signals or timers.
Use bufferevent when you are working with data streams and want the library to handle the buffering and I/O operations for you. This is often the case in network programming, where you need to send and receive data in chunks.
