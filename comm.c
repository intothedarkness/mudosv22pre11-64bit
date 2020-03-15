/*
 *  comm.c -- communications functions and more.
 *            Dwayne Fontenot (Jacques@TMI)
 */
#include "std.h"
#include "comm.h"
#include "main.h"
#include "socket_efuns.h"
#include "backend.h"
#include "socket_ctrl.h"
#include "debug.h"
#include "ed.h"
#include "file.h"
#include "master.h"
#include "add_action.h"

static char telnet_break_response[] = {  28, IAC, WILL, TELOPT_TM };
static char telnet_ip_response[]    = { 127, IAC, WILL, TELOPT_TM };
static char telnet_abort_response[] = { IAC, DM };
static char telnet_do_tm_response[] = { IAC, WILL, TELOPT_TM };
static char telnet_do_naws[]        = { IAC, DO, TELOPT_NAWS };
static char telnet_do_ttype[]       = { IAC, DO, TELOPT_TTYPE };
static char telnet_term_query[]     = { IAC, SB, TELOPT_TTYPE, TELQUAL_SEND, IAC, SE };
static char telnet_no_echo[]        = { IAC, WONT, TELOPT_ECHO };
static char telnet_no_single[]      = { IAC, WONT, TELOPT_SGA };
static char telnet_yes_echo[]       = { IAC, WILL, TELOPT_ECHO };
static char telnet_yes_single[]     = { IAC, WILL, TELOPT_SGA };
static char telnet_ga[]             = { IAC, GA };
static char telnet_ayt_response[]   = { '\n', '[', '-', 'Y', 'e', 's', '-', ']', ' ', '\n' };
static char telnet_line_mode[]      = { IAC, DO, TELOPT_LINEMODE };
static char telnet_lm_mode[]        = { IAC, SB, TELOPT_LINEMODE, LM_MODE, MODE_EDIT | MODE_TRAPSIG, IAC, SE };
static char telnet_char_mode[]	    = { IAC, DONT, TELOPT_LINEMODE };

static char slc_default_flags[] = { SLC_NOSUPPORT, SLC_CANTCHANGE, SLC_CANTCHANGE, SLC_CANTCHANGE, SLC_CANTCHANGE, SLC_NOSUPPORT,
				    SLC_NOSUPPORT, SLC_NOSUPPORT, SLC_CANTCHANGE, SLC_CANTCHANGE, SLC_NOSUPPORT, SLC_NOSUPPORT,
				    SLC_NOSUPPORT, SLC_NOSUPPORT, SLC_NOSUPPORT, SLC_NOSUPPORT, SLC_NOSUPPORT, SLC_NOSUPPORT };
static char slc_default_chars[] = { 0x00, BREAK, IP, AO, AYT, 0x00, 0x00, 0x00, SUSP, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
#ifdef DEBUG
static char *slc_names[] = { SLC_NAMELIST };
#endif

/*
 * local function prototypes.
 */
#ifdef SIGNAL_FUNC_TAKES_INT
static void sigpipe_handler PROT((int));
#else
static void sigpipe_handler PROT((void));
#endif

static void hname_handler PROT((void));
static void get_user_data PROT((interactive_t *));
static char *get_user_command PROT((void));
static char *first_cmd_in_buf PROT((interactive_t *));
static int cmd_in_buf PROT((interactive_t *));
static int call_function_interactive PROT((interactive_t *, char *));
static void print_prompt PROT((interactive_t *));
static void query_addr_name PROT((object_t *));
static void got_addr_number PROT((char *, char *));
static void add_ip_entry PROT((long, char *));
static void new_user_handler PROT((int));

#ifdef NO_SNOOP
#  define handle_snoop(str, len, who)
#else
#  define handle_snoop(str, len, who) if ((who)->snooped_by) receive_snoop(str, len, who->snooped_by)

static void receive_snoop PROT((char *, int, object_t * ob));

#endif

/*
 * public local variables.
 */
fd_set readmask, writemask;
int num_user;
#ifdef F_SET_HIDE
int num_hidden_users = 0;	/* for the O_HIDDEN flag.  This counter must
				 * be kept up to date at all times!  If you
				 * modify the O_HIDDEN flag in an object,
				 * make sure that you update this counter if
				 * the object is interactive. */
#endif
int add_message_calls = 0;
#ifdef F_NETWORK_STATS
int inet_out_packets = 0;
int inet_out_volume = 0;
int inet_in_packets = 0;
int inet_in_volume = 0;
#ifdef PACKAGE_SOCKETS
int inet_socket_in_packets = 0;
int inet_socket_in_volume = 0;
int inet_socket_out_packets = 0;
int inet_socket_out_volume = 0;
#endif
#endif
int inet_packets = 0;
int inet_volume = 0;
interactive_t **all_users = 0;
int max_users = 0;

/*
 * private local variables.
 */
static int addr_server_fd = -1;

static
void set_linemode P1(interactive_t *, ip)
{
    if (ip->iflags & USING_LINEMODE) {
	add_binary_message(ip->ob, telnet_line_mode, sizeof(telnet_line_mode));
	add_binary_message(ip->ob, telnet_lm_mode, sizeof(telnet_lm_mode));
    } else {
	add_binary_message(ip->ob, telnet_no_single, sizeof(telnet_no_single));
    }
}

static
void set_charmode P1(interactive_t *, ip)
{
    if (ip->iflags & USING_LINEMODE) {
	add_binary_message(ip->ob, telnet_char_mode, sizeof(telnet_char_mode));
    } else {
	add_binary_message(ip->ob, telnet_yes_single, sizeof(telnet_yes_single));
    }
}

#ifndef NO_SNOOP
static void
receive_snoop P3(char *, buf, int, len, object_t *, snooper)
{
    /* command giver no longer set to snooper */
#ifdef RECEIVE_SNOOP
    char *str;

    str = new_string(len, "receive_snoop");
    memcpy(str, buf, len);
    str[len] = 0;
    push_malloced_string(str);
    apply(APPLY_RECEIVE_SNOOP, snooper, 1, ORIGIN_DRIVER);
#else
    /* snoop output is now % in all cases */
    add_message(snooper, "%", 1);
    add_message(snooper, buf, len);
#endif
}
#endif

/*
 * Initialize new user connection socket.
 */
void init_user_conn()
{
    struct sockaddr_in sin;
    int sin_len;
    int optval;
    int i;
    int have_fd6;
    int fd6_which;
    
    /* Check for fd #6 open as a valid socket */
    optval = 1;
    have_fd6 = (setsockopt(6, SOL_SOCKET, SO_REUSEADDR, (char *)&optval, sizeof(optval)) == 0);
    
    for (i=0; i < 5; i++) {
#ifdef F_NETWORK_STATS
	external_port[i].in_packets = 0;
	external_port[i].in_volume = 0;
	external_port[i].out_packets = 0;
	external_port[i].out_volume = 0;
#endif
	if (!external_port[i].port) {
	    if (!have_fd6) continue;
	    fd6_which = i;
	    have_fd6 = 0;
	    if (FD6_KIND == PORT_UNDEFINED || FD6_PORT < 1) {
		debug_message("Socket passed to fd 6 ignored (support is disabled).\n");
		continue;
	    }

	    debug_message("Accepting connections on fd 6 (port %d).\n", FD6_PORT);
	    external_port[i].kind = FD6_KIND;
	    external_port[i].port = FD6_PORT;
	    external_port[i].fd = 6;
	} else {
	    /*
	     * create socket of proper type.
	     */
	    if ((external_port[i].fd = socket(PF_INET, SOCK_STREAM, 0)) == -1) {
		debug_perror("init_user_conn: socket", 0);
		exit(1);
	    }
	    
            /*
	     * enable local address reuse.
	     */
	    optval = 1;
	    if (setsockopt(external_port[i].fd, SOL_SOCKET, SO_REUSEADDR,
			   (char *) &optval, sizeof(optval)) == -1) {
		socket_perror("init_user_conn: setsockopt", 0);
		exit(2);
	    }

	    /*
	     * fill in socket address information.
	     */
	    sin.sin_family = AF_INET;
	    
	    if (MUD_IP[0]) sin.sin_addr.s_addr = inet_addr(MUD_IP);
	    else sin.sin_addr.s_addr = INADDR_ANY;
	    
	    sin.sin_port = htons((u_short) external_port[i].port);
	    /*
	     * bind name to socket.
	     */
	    if (bind(external_port[i].fd, (struct sockaddr *) & sin,
		     sizeof(sin)) == -1) {
		socket_perror("init_user_conn: bind", 0);
		exit(3);
	    }
	}
	/*
	 * get socket name.
	 */
	sin_len = sizeof(sin);
	if (getsockname(external_port[i].fd, (struct sockaddr *) & sin,
			&sin_len) == -1) {
	    socket_perror("init_user_conn: getsockname", 0);
	    if (i != fd6_which) {
		exit(4);
	    }
	}
	/*
	 * set socket non-blocking,
	 */
	if (set_socket_nonblocking(external_port[i].fd, 1) == -1) {
	    socket_perror("init_user_conn: set_socket_nonblocking 1", 0);
	    if (i != fd6_which) {
		exit(8);
	    }
	}
	/*
	 * listen on socket for connections.
	 */
	if (listen(external_port[i].fd, 128) == -1) {
	    socket_perror("init_user_conn: listen", 0);
	    if (i != fd6_which) {
		exit(10);
	    }
	}
    }
    if (have_fd6) {
	debug_message("No more ports available; fd #6 ignored.\n");
    }
    /*
     * register signal handler for SIGPIPE.
     */
#if !defined(LATTICE) && defined(SIGPIPE)
    if (signal(SIGPIPE, sigpipe_handler) == SIGNAL_ERROR) {
	debug_perror("init_user_conn: signal SIGPIPE",0);
	exit(5);
    }
#endif
}

/*
 * Shut down new user accept file descriptor.
 */
void ipc_remove()
{
    int i;

    for (i = 0; i < 5; i++) {
	if (!external_port[i].port) continue;
	if (OS_socket_close(external_port[i].fd) == -1) {
	    socket_perror("ipc_remove: close", 0);
	}
    }

    debug_message("closed external ports\n");
}

void init_addr_server P2(char *, hostname, int, addr_server_port)
{
    struct sockaddr_in server;
    struct hostent *hp;
    int server_fd;
    int optval;
    long addr;

    if (addr_server_fd >= 0)
	return;

    if (!hostname) return;

    /*
     * get network host data for hostname.
     */
    if (hostname[0] >= '0' && hostname[0] <= '9' &&
          (addr = inet_addr(hostname)) != -1) {
        hp = gethostbyaddr((char *)&addr, sizeof(addr), AF_INET);
    } else {
        hp = gethostbyname(hostname);
    }
    if (hp == NULL) {
	socket_perror("init_addr_server: gethostbyname", 0);
	return;
    }

    /*
     * set up address information for server.
     */
    server.sin_family = AF_INET;
    server.sin_port = htons((u_short) addr_server_port);
    server.sin_addr.s_addr = inet_addr(hostname);
    memcpy((char *) &server.sin_addr, (char *) hp->h_addr, hp->h_length);
    /*
     * create socket of proper type.
     */
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == INVALID_SOCKET) {	/* problem opening socket */
	socket_perror("init_addr_server: socket", 0);
	return;
    }
    /*
     * enable local address reuse.
     */
    optval = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (char *) &optval,
		   sizeof(optval)) == -1) {
	socket_perror("init_addr_server: setsockopt", 0);
	return;
    }
    /*
     * connect socket to server address.
     */
    if (connect(server_fd, (struct sockaddr *) & server, sizeof(server)) == -1) {
	if (socket_errno == ECONNREFUSED)
	    debug_message("Connection to address server (%s %d) refused.\n",
			  hostname, addr_server_port);
	else 
	    socket_perror("init_addr_server: connect", 0);
	OS_socket_close(server_fd);
	return;
    }
    addr_server_fd = server_fd;
    debug_message("Connected to address server on %s port %d\n", hostname,
	    addr_server_port);
    /*
     * set socket non-blocking.
     */
    if (set_socket_nonblocking(server_fd, 1) == -1) {
	socket_perror("init_addr_server: set_socket_nonblocking 1", 0);
	return;
    }
}

/*
 * If there is a shadow for this object, then the message should be
 * sent to it. But only if catch_tell() is defined. Beware that one of the
 * shadows may be the originator of the message, which means that we must
 * not send the message to that shadow, or any shadows in the linked list
 * before that shadow.
 *
 * Also note that we don't need to do this in the case of
 * INTERACTIVE_CATCH_TELL, since catch_tell() was already called
 * _instead of_ add_message(), and shadows got their chance then.
 */
#if !defined(INTERACTIVE_CATCH_TELL) && !defined(NO_SHADOWS)
#define SHADOW_CATCH_MESSAGE
#endif

#ifdef SHADOW_CATCH_MESSAGE
static int shadow_catch_message P2(object_t *, ob, char *, str)
{
    if (!ob->shadowed)
	return 0;
    while (ob->shadowed != 0 && ob->shadowed != current_object)
	ob = ob->shadowed;
    while (ob->shadowing) {
	copy_and_push_string(str);
	if (apply(APPLY_CATCH_TELL, ob, 1, ORIGIN_DRIVER))	
	    /* this will work, since we know the */
	    /* function is defined */
	    return 1;
	ob = ob->shadowing;
    }
    return 0;
}
#endif

/*
 * Send a message to an interactive object. If that object is shadowed,
 * special handling is done.
 */
void add_message P3(object_t *, who, char *, data, int, len)
{
    interactive_t *ip;
    char *cp;
    char *end;
    
    /*
     * if who->interactive is not valid, write message on stderr.
     * (maybe)
     */
    if (!who || (who->flags & O_DESTRUCTED) || !who->interactive ||
	(who->interactive->iflags & (NET_DEAD | CLOSING))) {
#ifdef NONINTERACTIVE_STDERR_WRITE
	putc(']', stderr);
	fwrite(data, len, 1, stderr);
#endif
#ifdef LATTICE
	fflush(stderr);
#endif
	return;
    }
    ip = who->interactive;
#ifdef SHADOW_CATCH_MESSAGE
    /*
     * shadow handling.
     */
    if (shadow_catch_message(who, data)) {
#ifdef SNOOP_SHADOWED
	handle_snoop(data, len, ip);
#endif
	return;
    }
#endif				/* NO_SHADOWS */

    /*
     * write message into ip->message_buf.
     */
    end = data + len;
    for (cp = data; cp < end; cp++) {
	if (ip->message_length == MESSAGE_BUF_SIZE) {
	    if (!flush_message(ip)) {
		debug(connections, ("Broken connection during add_message."));
		return;
	    }
	    if (ip->message_length == MESSAGE_BUF_SIZE)
		break;
	}
	if (*cp == '\n'
#ifndef NO_BUFFER_TYPE
	    && ip->connection_type != PORT_BINARY
#endif
	    ) {
	    if (ip->message_length == (MESSAGE_BUF_SIZE - 1)) {
		if (!flush_message(ip)) {
		    debug(connections, ("Broken connection during add_message."));
		    return;
		}
		if (ip->message_length == (MESSAGE_BUF_SIZE - 1))
		    break;
	    }
	    ip->message_buf[ip->message_producer] = '\r';
	    ip->message_producer = (ip->message_producer + 1)
		% MESSAGE_BUF_SIZE;
	    ip->message_length++;
	}
	ip->message_buf[ip->message_producer] = *cp;
	ip->message_producer = (ip->message_producer + 1) % MESSAGE_BUF_SIZE;
	ip->message_length++;
    }

    handle_snoop(data, len, ip);

#ifdef FLUSH_OUTPUT_IMMEDIATELY
    flush_message(ip);
#endif
    
    add_message_calls++;
}				/* add_message() */

/* WARNING: this can only handle results < LARGEST_PRINTABLE_STRING in size */
void add_vmessage P2V(object_t *, who, char *, format)
{
    int len;
    interactive_t *ip;
    char *cp, new_string_data[LARGEST_PRINTABLE_STRING + 1];
    va_list args;
    V_DCL(char *format);
    V_DCL(object_t *who);

    V_START(args, format);
    V_VAR(object_t *, who, args);
    V_VAR(char *, format, args);
    /*
     * if who->interactive is not valid, write message on stderr.
     * (maybe)
     */
    if (!who || (who->flags & O_DESTRUCTED) || !who->interactive || 
	(who->interactive->iflags & (NET_DEAD | CLOSING))) {
#ifdef NONINTERACTIVE_STDERR_WRITE
	putc(']', stderr);
	vfprintf(stderr, format, args);
#endif
	va_end(args);
#ifdef LATTICE
	fflush(stderr);
#endif
	return;
    }
    ip = who->interactive;
    new_string_data[0] = '\0';
    /*
     * this is dangerous since the data may not all fit into new_string_data
     * but how to tell if it will without trying it first?  I suppose one
     * could rewrite vsprintf to accept a maximum length (like strncpy) --
     * have fun!
     */
    vsprintf(new_string_data, format, args);
    va_end(args);
    len = strlen(new_string_data);
#ifdef SHADOW_CATCH_MESSAGE
    /*
     * shadow handling.
     */
    if (shadow_catch_message(who, new_string_data)) {
#ifdef SNOOP_SHADOWED
	handle_snoop(new_string_data, len, ip);
#endif
	return;
    }
#endif				/* NO_SHADOWS */

    /*
     * write message into ip->message_buf.
     */
    for (cp = new_string_data; *cp != '\0'; cp++) {
	if (ip->message_length == MESSAGE_BUF_SIZE) {
	    if (!flush_message(ip)) {
		debug(connections, ("Broken connection during add_message."));
		return;
	    }
	    if (ip->message_length == MESSAGE_BUF_SIZE)
		break;
	}
	if (*cp == '\n') {
	    if (ip->message_length == (MESSAGE_BUF_SIZE - 1)) {
		if (!flush_message(ip)) {
		    debug(connections, ("Broken connection during add_message.\n"));
		    return;
		}
		if (ip->message_length == (MESSAGE_BUF_SIZE - 1))
		    break;
	    }
	    ip->message_buf[ip->message_producer] = '\r';
	    ip->message_producer = (ip->message_producer + 1)
		% MESSAGE_BUF_SIZE;
	    ip->message_length++;
	}
	ip->message_buf[ip->message_producer] = *cp;
	ip->message_producer = (ip->message_producer + 1) % MESSAGE_BUF_SIZE;
	ip->message_length++;
    }
    if (ip->message_length != 0) {
	if (!flush_message(ip)) {
	    debug(connections, ("Broken connection during add_message.\n"));
	    return;
	}
    }

    handle_snoop(new_string_data, len, ip);

#ifdef FLUSH_OUTPUT_IMMEDIATELY
    flush_message(ip);
#endif
    
    add_message_calls++;
}				/* add_message() */

void add_binary_message P3(object_t *, who, char *, data, int, len)
{
    interactive_t *ip;
    char *cp, *end;
    
    /*
     * if who->interactive is not valid, bail
     */
    if (!who || (who->flags & O_DESTRUCTED) || !who->interactive ||
	(who->interactive->iflags & (NET_DEAD | CLOSING))) {
	return;
    }
    ip = who->interactive;

    /*
     * write message into ip->message_buf.
     */
    end = data + len;
    for (cp = data; cp < end; cp++) {
	if (ip->message_length == MESSAGE_BUF_SIZE) {
	    if (!flush_message(ip)) {
		debug(connections, ("Broken connection during add_message."));
		return;
	    }
	    if (ip->message_length == MESSAGE_BUF_SIZE)
		break;
	}
	ip->message_buf[ip->message_producer] = *cp;
	ip->message_producer = (ip->message_producer + 1) % MESSAGE_BUF_SIZE;
	ip->message_length++;
    }

    flush_message(ip);
    add_message_calls++;
}

/*
 * Flush outgoing message buffer of current interactive object.
 */
int flush_message P1(interactive_t *, ip)
{
    int length, num_bytes;

    /*
     * if ip is not valid, do nothing.
     */
    if (!ip || (ip->iflags & (NET_DEAD | CLOSING))) {
	debug(connections, ("flush_message: invalid target!\n"));
	return 0;
    }
    /*
     * write ip->message_buf[] to socket.
     */
    while (ip->message_length != 0) {
	if (ip->message_consumer < ip->message_producer) {
	    length = ip->message_producer - ip->message_consumer;
	} else {
	    length = MESSAGE_BUF_SIZE - ip->message_consumer;
	}
/* Need to use send to get out of band data
   num_bytes = write(ip->fd,ip->message_buf + ip->message_consumer,length);
 */
	num_bytes = send(ip->fd, ip->message_buf + ip->message_consumer,
			 length, ip->out_of_band);
	if (!num_bytes) {
	    ip->iflags |= NET_DEAD;
	    return 0;
	}
	if (num_bytes == -1) {
#ifdef EWOULDBLOCK
	    if (socket_errno == EWOULDBLOCK) {
		debug(connections, ("flush_message: write: Operation would block\n"));
		return 1;
#else
	    if (0) {
		;
#endif
	    } else if (socket_errno == EINTR) {
		debug(connections, ("flush_message: write: Interrupted system call"));
		return 1;
	    } else {
		socket_perror("flush_message: write", 0);
		ip->iflags |= NET_DEAD;
		return 0;
	    }
	}
	ip->message_consumer = (ip->message_consumer + num_bytes) %
	    MESSAGE_BUF_SIZE;
	ip->message_length -= num_bytes;
	ip->out_of_band = 0;
	inet_packets++;
	inet_volume += num_bytes;
#ifdef F_NETWORK_STATS
	inet_out_packets++;
	inet_out_volume += num_bytes;
	external_port[ip->external_port].out_packets++;
	external_port[ip->external_port].out_volume += num_bytes;
#endif
    }
    return 1;
}				/* flush_message() */

static void copy_chars P3(interactive_t *, ip, char *, from, int, num_bytes)
{
    int i, start, x;
    char dont_response[3] = { IAC, DONT, 0 };
    char wont_response[3] = { IAC, WONT, 0 };

    start = ip->text_end;
    for (i = 0;  i < num_bytes;  i++) {
	switch (ip->state) {
	    case TS_DATA:
		switch ((unsigned char)from[i]) {
		    case IAC:
			ip->state = TS_IAC;
			break;

#if defined(NO_ANSI) && defined(STRIP_BEFORE_PROCESS_INPUT)
		    case 0x1b:
			ip->text[ip->text_end++] = 0x20;
			break;
#endif

		    case 0x08:
		    case 0x7f:
			if (ip->iflags & SINGLE_CHAR)
			    ip->text[ip->text_end++] = from[i];
			else {
			    if (ip->text_end > 0)
				ip->text_end--;
			}
			break;

		    default:
			ip->text[ip->text_end++] = from[i];
			break;
		}
		break;

	    case TS_IAC:
		switch ((unsigned char)from[i]) {
		    case IAC:
			ip->state = TS_DATA;
			ip->text[ip->text_end++] = from[i];
			break;

		    case WILL:
			ip->state = TS_WILL;
			break;

		    case WONT:
			ip->state = TS_WONT;
			break;

		    case DO:
			ip->state = TS_DO;
			break;

		    case DONT:
			ip->state = TS_DONT;
			break;

		    case SB:
			ip->state = TS_SB;
			ip->sb_pos = 0;
			break;

		    case BREAK:
			add_binary_message(ip->ob, telnet_break_response, sizeof(telnet_break_response));
			break;

		    case IP:	/* interrupt process */
			add_binary_message(ip->ob, telnet_ip_response, sizeof(telnet_ip_response));
			break;

		    case AYT:	/* are you there?  you bet */
			add_binary_message(ip->ob, telnet_ayt_response, sizeof(telnet_ayt_response));
			break;

		    case AO:	/* abort output */
			flush_message(ip);
			ip->out_of_band = MSG_OOB;
			add_binary_message(ip->ob, telnet_abort_response, sizeof(telnet_abort_response));
			break;

		    default:
			ip->state = TS_DATA;
			break;
		}
		break;

	    case TS_WILL:
		ip->iflags |= USING_TELNET;
		switch ((unsigned char)from[i]) {
		    case TELOPT_TTYPE:
			add_binary_message(ip->ob, telnet_term_query, sizeof(telnet_term_query));
			break;

		    case TELOPT_LINEMODE:
			/* Do linemode and set the mode: EDIT + TRAPSIG */
			ip->iflags |= USING_LINEMODE;
			set_linemode(ip);
			break;

		    case TELOPT_ECHO:
		    case TELOPT_NAWS:
			/* do nothing, but don't send a dont response */
			break;

		    default:
			dont_response[2] = from[i];
			add_binary_message(ip->ob, dont_response, sizeof(dont_response));
			break;
		}
		ip->state = TS_DATA;
		break;

	    case TS_WONT:
		ip->iflags |= USING_TELNET;
		switch ((unsigned char)from[i]) {
		    case TELOPT_LINEMODE:
			/* If we're in single char mode, we just requested for
			 * linemode to be disabled, so don't remove our flag.
			 */
			if (!(ip->iflags & SINGLE_CHAR))
			    ip->iflags &= ~USING_LINEMODE;
			break;
		}
		ip->state = TS_DATA;
		break;

	    case TS_DO:
		switch ((unsigned char)from[i]) {
		    case TELOPT_TM:
			add_binary_message(ip->ob, telnet_do_tm_response, sizeof(telnet_do_tm_response));
			break;

		    case TELOPT_SGA:
			if (ip->iflags & USING_LINEMODE) {
			    ip->iflags |= SUPPRESS_GA;
			    add_binary_message(ip->ob, telnet_yes_single, sizeof(telnet_yes_single));
			} else {
			    if (ip->iflags & SINGLE_CHAR)
				add_binary_message(ip->ob, telnet_yes_single, sizeof(telnet_yes_single));
			    else
				add_binary_message(ip->ob, telnet_no_single, sizeof(telnet_no_single));
			}
			break;

		    case TELOPT_ECHO:
			/* do nothing, but don't send a wont response */
			break;

		    default:
			wont_response[2] = from[i];
			add_binary_message(ip->ob, wont_response, sizeof(wont_response));
			break;
		}
		ip->state = TS_DATA;
		break;

	    case TS_DONT:
		switch ((unsigned char)from[i]) {
		    case TELOPT_SGA:
			if (ip->iflags & USING_LINEMODE) {
			    ip->iflags &= ~SUPPRESS_GA;
			    add_binary_message(ip->ob, telnet_no_single, sizeof(telnet_no_single));
			}
			break;
		}
		ip->state = TS_DATA;
		break;

	    case TS_SB:
		if ((unsigned char)from[i] == IAC) {
		    ip->state = TS_SB_IAC;
		    break;
		}
		if (ip->sb_pos < SB_SIZE - 1)
		    ip->sb_buf[ip->sb_pos++] = from[i];
		break;

	    case TS_SB_IAC:
		switch ((unsigned char)from[i]) {
		    case IAC:
			if (ip->sb_pos < SB_SIZE - 1) {
			    ip->sb_buf[ip->sb_pos++] = from[i];
			    ip->state = TS_SB;
			}
			break;

		    case SE:
			ip->state = TS_DATA;
			ip->sb_buf[ip->sb_pos] = 0;
			switch (ip->sb_buf[0]) {
			    case TELOPT_LINEMODE:
				switch ((unsigned char)ip->sb_buf[1]) {
				    case LM_MODE:
					/* Don't do anything with an ACK */
					if (!(ip->sb_buf[2] & MODE_ACK)) {
					    char sb_ack[] = { IAC, SB, TELOPT_LINEMODE, LM_MODE, MODE_EDIT | MODE_TRAPSIG | MODE_ACK, IAC, SE };

					    /* Accept only EDIT and TRAPSIG && force them too */
					    add_binary_message(ip->ob, sb_ack, sizeof(sb_ack));
					}
					break;

				    case LM_SLC:
					{
					    int slc_length = 4;
					    char slc_response[SB_SIZE + 6] = { IAC, SB, TELOPT_LINEMODE, LM_SLC };

					    for (x = 2;  x < ip->sb_pos;  x += 3) {
						/* no response for an ack */
						if (ip->sb_buf[x + 1] & SLC_ACK)
						    continue;

						/* If we get { 0, SLC_DEFAULT, 0 } or { 0, SLC_VARIABLE, 0 } return a list of values */
						/* If it's SLC_DEFAULT, reset to defaults first */
						if (!ip->sb_buf[x] && !ip->sb_buf[x + 2]) {
						    if (ip->sb_buf[x + 1] == SLC_DEFAULT || ip->sb_buf[x + 1] == SLC_VARIABLE) {
							int n;

							for (n = 0;  n < NSLC;  n++) {
							    slc_response[slc_length++] = n + 1;
							    if (ip->sb_buf[x + 1] == SLC_DEFAULT) {
								ip->slc[n][0] = slc_default_flags[n];
								ip->slc[n][1] = slc_default_chars[n];
								slc_response[slc_length++] = SLC_DEFAULT;
							    } else {
								slc_response[slc_length++] = ip->slc[n][0];
							    }
							    slc_response[slc_length++] = ip->slc[n][1];
							}
							break;
						    }
						}

						slc_response[slc_length++] = ip->sb_buf[x]--;

						/* If the first octet is out of range, we don't support it */
						/* If the default flag is not supported, we don't support it */
						if (ip->sb_buf[x] >= NSLC || slc_default_flags[(int)ip->sb_buf[x]] == SLC_NOSUPPORT) {
						    slc_response[slc_length++] = SLC_NOSUPPORT;
						    slc_response[slc_length++] = ip->sb_buf[x + 2];
						    if ((unsigned char)ip->sb_buf[x + 2] == IAC)
							slc_response[slc_length++] = IAC;
						    continue;
						}

						switch ((ip->sb_buf[x + 1] & SLC_LEVELBITS)) {
						    case SLC_NOSUPPORT:
							if (slc_default_flags[(int)ip->sb_buf[x]] == SLC_CANTCHANGE) {
							    slc_response[slc_length++] = SLC_CANTCHANGE;
							    slc_response[slc_length++] = ip->slc[(int)ip->sb_buf[x]][1];
							    break;
							}
							slc_response[slc_length++] = SLC_ACK | SLC_NOSUPPORT;
							slc_response[slc_length++] = ip->sb_buf[x + 2];
							ip->slc[(int)ip->sb_buf[x]][0] = SLC_NOSUPPORT;
							ip->slc[(int)ip->sb_buf[x]][1] = 0;
							break;

						    case SLC_VARIABLE:
							if (slc_default_flags[(int)ip->sb_buf[x]] == SLC_CANTCHANGE) {
							    slc_response[slc_length++] = SLC_CANTCHANGE;
							    slc_response[slc_length++] = ip->slc[(int)ip->sb_buf[x]][1];
							    break;
							}
							slc_response[slc_length++] = SLC_ACK | SLC_VARIABLE;
							slc_response[slc_length++] = ip->sb_buf[x + 2];
							ip->slc[(int)ip->sb_buf[x]][0] = ip->sb_buf[x + 1];
							ip->slc[(int)ip->sb_buf[x]][1] = ip->sb_buf[x + 2];
							break;

						    case SLC_CANTCHANGE:
							slc_response[slc_length++] = SLC_ACK | SLC_CANTCHANGE;
							slc_response[slc_length++] = ip->sb_buf[x + 2];
							ip->slc[(int)ip->sb_buf[x]][0] = ip->sb_buf[x + 1];
							ip->slc[(int)ip->sb_buf[x]][1] = ip->sb_buf[x + 2];
							break;

						    case SLC_DEFAULT:
							slc_response[slc_length++] = slc_default_flags[(int)ip->sb_buf[x]];
							slc_response[slc_length++] = slc_default_flags[(int)ip->sb_buf[x]];
							ip->slc[(int)ip->sb_buf[x]][0] = slc_default_flags[(int)ip->sb_buf[x]];
							ip->slc[(int)ip->sb_buf[x]][1] = slc_default_chars[(int)ip->sb_buf[x]];
							break;

						    default:
							slc_response[slc_length++] = SLC_NOSUPPORT;
							slc_response[slc_length++] = ip->sb_buf[x + 2];
							if ((unsigned char)slc_response[slc_length - 1] == IAC)
							    slc_response[slc_length++] = IAC;
							break;
						}
					    }

					    if (slc_length > 4) {
						/* send our response */
						slc_response[slc_length++] = IAC;
						slc_response[slc_length++] = SE;
						add_binary_message(ip->ob, slc_response, slc_length);
					    }
					}
					break;

				    case DO:
					{
					    char sb_wont[] = { IAC, SB, TELOPT_LINEMODE, WONT, 0, IAC, SE };

					    /* send back IAC SB TELOPT_LINEMODE WONT x IAC SE */
					    sb_wont[4] = ip->sb_buf[2];
					    add_binary_message(ip->ob, sb_wont, sizeof(sb_wont));
					}
					break;

				    case WILL:
					{
					    char sb_dont[] = { IAC, SB, TELOPT_LINEMODE, DONT, 0, IAC, SE };

					    /* send back IAC SB TELOPT_LINEMODE DONT x IAC SE */
					    sb_dont[4] = ip->sb_buf[2];
					    add_binary_message(ip->ob, sb_dont, sizeof(sb_dont));
					}
					break;
				}
				break;

			    case TELOPT_NAWS:
				if (ip->sb_pos >= 5) {
				    push_number(((unsigned char)ip->sb_buf[1] << 8) | (unsigned char)ip->sb_buf[2]);
				    push_number(((unsigned char)ip->sb_buf[3] << 8) | (unsigned char)ip->sb_buf[4]);
				    apply(APPLY_WINDOW_SIZE, ip->ob, 2, ORIGIN_DRIVER);
				}
				break;

			    case TELOPT_TTYPE:
				if (!ip->sb_buf[1]) {
				    copy_and_push_string(ip->sb_buf + 2);
				    apply(APPLY_TERMINAL_TYPE, ip->ob, 1, ORIGIN_DRIVER);
				}
			       	break;

			    default:
				for (x = 0;  x < ip->sb_pos;  x++)
				    ip->sb_buf[x] = (ip->sb_buf[x] ? ip->sb_buf[x] : 'I');
				copy_and_push_string(ip->sb_buf);
				apply(APPLY_TELNET_SUBOPTION, ip->ob, 1, ORIGIN_DRIVER);
				break;
			}
			break;

		    default:
			/*
			 * Apparently, old versions of MUD-OS would revert to TS_DATA here.
			 * Later versions handle the IAC, and then go back to TS_DATA mode.
			 * I don't think either is proper, but the related RFC documents
			 * aren't clear on what to do (854, 855).  It is my feeling, that
			 * the safest thing to do here is to ignore the option.
			 * -- Marius, 6-Jun-2000
			 */
			ip->state = TS_SB;
			break;
		}
		break;
	}
    }

    if (ip->text_end > start) {
	/* handle snooping - snooper does not see type-ahead due to
	   telnet being in linemode */
	if (!(ip->iflags & NOECHO))
	    handle_snoop(ip->text + start, ip->text_end - start, ip);
    }
}

/*
 * Read pending data for a user into user->interactive->text.
 * This also does telnet negotiation.
 */
static void get_user_data P1(interactive_t *, ip)
{
    int  num_bytes, text_space;
    char buf[MAX_TEXT];

    /* compute how much data we can read right now */
    switch (ip->connection_type)
    {
	case PORT_TELNET:
	    text_space = MAX_TEXT - ip->text_end;

	    /* check if we need more space */
	    if (text_space < MAX_TEXT / 16) {
		if (ip->text_start > 0) {
		    memmove(ip->text, ip->text + ip->text_start, ip->text_end - ip->text_start);
		    text_space += ip->text_start;
		    ip->text_end -= ip->text_start;
		    ip->text_start = 0;
		}

		if (text_space < MAX_TEXT / 16) {
		    /* the user is sending too much data.  flush it */
		    ip->iflags |= SKIP_COMMAND;
		    ip->text_start = ip->text_end = 0;
		    text_space = MAX_TEXT;
		}
	    }
	    break;

	case PORT_MUD:
	    if (ip->text_end < 4)
		text_space = 4 - ip->text_end;
	    else
		text_space = *(int *)ip->text - ip->text_end + 4;
	    break;

	default:
	    text_space = sizeof(buf);
	    break;
    }

    /* read the data from the socket */
    debug(connections, ("get_user_data: read on fd %d\n", ip->fd));
    num_bytes = OS_socket_read(ip->fd, buf, text_space);

    if (!num_bytes) {
	if (ip->iflags & CLOSING)
	    debug_message("get_user_data: tried to read from closing fd.\n");
	ip->iflags |= NET_DEAD;
	remove_interactive(ip->ob, 0);
	return;
    }

    if (num_bytes == -1) {
#ifdef EWOULDBLOCK
	if (socket_errno == EWOULDBLOCK) {
	    debug(connections, ("get_user_data: read on fd %d: Operation would block.\n", ip->fd));
	    return;
	}
#endif
	debug_message("get_user_data: read on fd %d\n", ip->fd);
	socket_perror("get_user_data: read", 0);
	ip->iflags |= NET_DEAD;
	remove_interactive(ip->ob, 0);
	return;
    }

#ifdef F_NETWORK_STATS
    inet_in_packets++;
    inet_in_volume += num_bytes;
    external_port[ip->external_port].in_packets++;
    external_port[ip->external_port].in_volume += num_bytes;
#endif

    /* process the data that we've just read */
    switch (ip->connection_type)
    {
	case PORT_TELNET:
	    copy_chars(ip, buf, num_bytes);
	    if (cmd_in_buf(ip))
		ip->iflags |= CMD_IN_BUF;
	    break;

	case PORT_MUD:
	    memcpy(ip->text + ip->text_end, buf, num_bytes);
	    ip->text_end += num_bytes;

	    if (num_bytes == text_space) {
		if (ip->text_end == 4) {
		    *(int *)ip->text = ntohl(*(int *)ip->text);
		    if (*(int *)ip->text > MAX_TEXT - 5)
			remove_interactive(ip->ob, 0);
		} else {
		    svalue_t value;

		    ip->text[ip->text_end] = 0;
		    if (restore_svalue(ip->text + 4, &value) == 0) {
			STACK_INC;
			*sp = value;
		    } else {
			push_undefined();
		    }
		    ip->text_end = 0;
		    apply(APPLY_PROCESS_INPUT, ip->ob, 1, ORIGIN_DRIVER);
		}
	    }
	    break;

	case PORT_ASCII:
	    {
		char *nl, *p;

		memcpy(ip->text + ip->text_end, buf, num_bytes);
		ip->text_end += num_bytes;

		p = ip->text + ip->text_start;
		while ((nl = memchr(p, '\n', ip->text_end - ip->text_start))) {
		    ip->text_start = (nl + 1) - ip->text;

		    *nl = 0;
		    if (*(nl - 1) == '\r')
			*--nl = 0;

		    if (!(ip->ob->flags & O_DESTRUCTED)) {
			char *str;
			
			str = new_string(nl - p, "PORT_ASCII");
    			memcpy(str, p, nl - p + 1);
			push_malloced_string(str);
			apply(APPLY_PROCESS_INPUT, ip->ob, 1, ORIGIN_DRIVER);
		    }

		    if (ip->text_start == ip->text_end) {
			ip->text_start = ip->text_end = 0;
			break;
		    }

		    p = nl + 1;
		}
	    }
	    break;

#ifndef NO_BUFFER_TYPE
	case PORT_BINARY:
	    {
		buffer_t *buffer;

		buffer = allocate_buffer(num_bytes);
		memcpy(buffer->item, buf, num_bytes);

		push_refed_buffer(buffer);
		apply(APPLY_PROCESS_INPUT, ip->ob, 1, ORIGIN_DRIVER);
	    }
	    break;
#endif
    }
}

static int clean_buf P1(interactive_t *, ip)
{
    /* skip null input */
    while (ip->text_start < ip->text_end && !*(ip->text + ip->text_start))
	ip->text_start++;

    /* if we've advanced beyond the end of the buffer, reset it */
    if (ip->text_start >= ip->text_end) {
	ip->text_start = ip->text_end = 0;
    }

    /* if we're skipping the current command, check to see if it has been
       completed yet.  if it has, flush it and clear the skip bit */
    if (ip->iflags & SKIP_COMMAND) {
	char *p;

	for (p = ip->text + ip->text_start;  p < ip->text + ip->text_end;  p++) {
	    if (*p == '\r' || *p == '\n') {
		ip->text_start += p - (ip->text + ip->text_start) + 1;
		ip->iflags &= ~SKIP_COMMAND;
		return clean_buf(ip);
	    }
	}
    }

    return (ip->text_end > ip->text_start);
}

static int cmd_in_buf P1(interactive_t *, ip)
{
    char *p;

    /* do standard input buffer cleanup */
    if (!clean_buf(ip))
	return 0;

    /* if we're in single character mode, we've got input */
    if (ip->iflags & SINGLE_CHAR)
	return 1;

    /* search for a newline.  if found, we have a command */
    for (p = ip->text + ip->text_start;  p < ip->text + ip->text_end;  p++) {
	if (*p == '\r' || *p == '\n')
	    return 1;
    }

    /* duh, no command */
    return 0;
}

static char *first_cmd_in_buf P1(interactive_t *, ip)
{
    char *p;
#ifdef GET_CHAR_IS_BUFFERED
    static char tmp[2];
#endif

    /* do standard input buffer cleanup */
    if (!clean_buf(ip))
	return 0;

    p = ip->text + ip->text_start;

    /* if we're in single character mode, we've got input */
    if (ip->iflags & SINGLE_CHAR) {
	if (*p == 8 || *p == 127)
	    *p = 0;
#ifndef GET_CHAR_IS_BUFFERED
	ip->text_start++;
	if (!clean_buf(ip))
	    ip->iflags &= ~CMD_IN_BUF;
	return p;
#else
	tmp[0] = *p;
	ip->text[ip->text_start++] = 0;
	if (!clean_buf(ip))
	    ip->iflags &= ~CMD_IN_BUF;
	return tmp;
#endif
    }

    /* search for the newline */
    while (ip->text[ip->text_start] != '\n' && ip->text[ip->text_start] != '\r')
	ip->text_start++;

    /* check for "\r\n" or "\n\r" */
    if (ip->text_start + 1 < ip->text_end &&
	((ip->text[ip->text_start] == '\r' && ip->text[ip->text_start + 1] == '\n') ||
	 (ip->text[ip->text_start] == '\n' && ip->text[ip->text_start + 1] == '\r'))) {
	ip->text[ip->text_start++] = 0;
    }
    
    ip->text[ip->text_start++] = 0;
    if (!cmd_in_buf(ip))
	ip->iflags &= ~CMD_IN_BUF;

    return p;
}
/*
 * SIGPIPE handler -- does very little for now.
 */
#ifdef SIGNAL_FUNC_TAKES_INT
void sigpipe_handler P1(int, sig)
#else
void sigpipe_handler()
#endif
{
    debug(connections, ("SIGPIPE received."));
    signal(SIGPIPE, sigpipe_handler);
}				/* sigpipe_handler() */

/*
 * SIGALRM handler.
 */
#ifdef SIGNAL_FUNC_TAKES_INT
void sigalrm_handler P1(int, sig)
#else
void sigalrm_handler()
#endif
{
    heart_beat_flag = 1;
}				/* sigalrm_handler() */

INLINE void make_selectmasks()
{
    int i;

    /*
     * generate readmask and writemask for select() call.
     */
    FD_ZERO(&readmask);
    FD_ZERO(&writemask);
    /*
     * set new user accept fd in readmask.
     */
    for (i = 0; i < 5; i++) {
	if (!external_port[i].port) continue;
	FD_SET(external_port[i].fd, &readmask);
    }
    /*
     * set user fds in readmask.
     */
    for (i = 0; i < max_users; i++) {
	if (!all_users[i] || (all_users[i]->iflags & (CLOSING | CMD_IN_BUF)))
	    continue;
	/*
	 * if this user needs more input to make a complete command, set his
	 * fd so we can get it.
	 */
	FD_SET(all_users[i]->fd, &readmask);
	if (all_users[i]->message_length != 0)
	    FD_SET(all_users[i]->fd, &writemask);
    }
    /*
     * if addr_server_fd is set, set its fd in readmask.
     */
    if (addr_server_fd >= 0) {
	FD_SET(addr_server_fd, &readmask);
    }
#if defined(PACKAGE_SOCKETS) || defined(PACKAGE_EXTERNAL)
    /*
     * set fd's for efun sockets.
     */
    for (i = 0; i < max_lpc_socks; i++) {
	if (lpc_socks[i].state != STATE_CLOSED) {
	    if (lpc_socks[i].state != STATE_FLUSHING &&
		(lpc_socks[i].flags & S_WACCEPT) == 0)
		FD_SET(lpc_socks[i].fd, &readmask);
	    if (lpc_socks[i].flags & S_BLOCKED)
		FD_SET(lpc_socks[i].fd, &writemask);
	}
    }
#endif
}				/* make_selectmasks() */

/*
 * Process I/O.
 */
INLINE void process_io()
{
    int i;

    /*
     * check for new user connection.
     */
    for (i = 0; i < 5; i++) {
	if (!external_port[i].port) continue;
	if (FD_ISSET(external_port[i].fd, &readmask)) {
	    debug(connections, ("process_io: NEW_USER\n"));
	    new_user_handler(i);
	}
    }
    /*
     * check for data pending on user connections.
     */
    for (i = 0; i < max_users; i++) {
	if (!all_users[i] || (all_users[i]->iflags & (CLOSING | CMD_IN_BUF)))
	    continue;
	if (all_users[i]->iflags & NET_DEAD) {
	    remove_interactive(all_users[i]->ob, 0);
	    continue;
	}
	if (FD_ISSET(all_users[i]->fd, &readmask)) {
	    debug(connections, ("process_io: USER %d\n", i));
	    get_user_data(all_users[i]);
	    if (!all_users[i])
		continue;
	}
	if (FD_ISSET(all_users[i]->fd, &writemask))
	    flush_message(all_users[i]);
    }
#if defined(PACKAGE_SOCKETS) || defined(PACKAGE_EXTERNAL)
    /*
     * check for data pending on efun socket connections.
     */
    for (i = 0; i < max_lpc_socks; i++) {
	if (lpc_socks[i].state != STATE_CLOSED)
	    if (FD_ISSET(lpc_socks[i].fd, &readmask))
		socket_read_select_handler(i);
	if (lpc_socks[i].state != STATE_CLOSED)
	    if (FD_ISSET(lpc_socks[i].fd, &writemask))
		socket_write_select_handler(i);
    }
#endif
    /*
     * check for data pending from address server.
     */
    if (addr_server_fd >= 0) {
	if (FD_ISSET(addr_server_fd, &readmask)) {
	    debug(connections, ("process_io: IP_DAEMON\n"));
	    hname_handler();
	}
    }
}

/*
 * This is the new user connection handler. This function is called by the
 * event handler when data is pending on the listening socket (new_user_fd).
 * If space is available, an interactive data structure is initialized and
 * the user is connected.
 */
static void new_user_handler P1(int, which)
{
    int new_socket_fd;
    struct sockaddr_in addr;
    int length;
    int i, x;
    object_t *master, *ob;
    svalue_t *ret;

    length = sizeof(addr);
    debug(connections, ("new_user_handler: accept on fd %d\n", external_port[which].fd));
    new_socket_fd = accept(external_port[which].fd,
			   (struct sockaddr *) & addr, (int *) &length);
    if (new_socket_fd < 0) {
#ifdef EWOULDBLOCK
	if (socket_errno == EWOULDBLOCK) {
	    debug(connections, ("new_user_handler: accept: Operation would block\n"));
	} else {
#else
	if (1) {
#endif
	    socket_perror("new_user_handler: accept", 0);
	}
	return;
    }

    /*
     * according to Amylaar, 'accepted' sockets in Linux 0.99p6 don't
     * properly inherit the nonblocking property from the listening socket.
     * Marius, 19-Jun-2000: this happens on other platforms as well, so just
     * do it for everyone
     */
    if (set_socket_nonblocking(new_socket_fd, 1) == -1) {
	socket_perror("new_user_handler: set_socket_nonblocking 1", 0);
	OS_socket_close(new_socket_fd);
	return;
    }

    /* find the first available slot */
    for (i = 0; i < max_users; i++)
	if (!all_users[i]) break;

    if (i == max_users) {
	if (all_users) {
	    all_users = RESIZE(all_users, max_users + 10, interactive_t *,
			       TAG_USERS, "new_user_handler");
	} else {
	    all_users = CALLOCATE(10, interactive_t *,
				  TAG_USERS, "new_user_handler");
	}
	while (max_users < i + 10)
	    all_users[max_users++] = 0;
    }

    set_command_giver(master_ob);
    master_ob->interactive =
	(interactive_t *)
	    DXALLOC(sizeof(interactive_t), TAG_INTERACTIVE,
		    "new_user_handler");
#ifndef NO_ADD_ACTION
    master_ob->interactive->default_err_message.s = 0;
#endif
    master_ob->interactive->connection_type = external_port[which].kind;
    master_ob->flags |= O_ONCE_INTERACTIVE;
    /*
     * initialize new user interactive data structure.
     */
    master_ob->interactive->ob = master_ob;
#if defined(F_INPUT_TO) || defined(F_GET_CHAR)
    master_ob->interactive->input_to = 0;
#endif
    master_ob->interactive->iflags = 0;
    master_ob->interactive->text[0] = '\0';
    master_ob->interactive->text_end = 0;
    master_ob->interactive->text_start = 0;
#if defined(F_INPUT_TO) || defined(F_GET_CHAR)
    master_ob->interactive->carryover = NULL;
    master_ob->interactive->num_carry = 0;
#endif
#ifndef NO_SNOOP
    master_ob->interactive->snooped_by = 0;
#endif
    master_ob->interactive->last_time = current_time;
#ifdef TRACE
    master_ob->interactive->trace_level = 0;
    master_ob->interactive->trace_prefix = 0;
#endif
#ifdef OLD_ED
    master_ob->interactive->ed_buffer = 0;
#endif
    master_ob->interactive->message_producer = 0;
    master_ob->interactive->message_consumer = 0;
    master_ob->interactive->message_length = 0;
    master_ob->interactive->state = TS_DATA;
    master_ob->interactive->out_of_band = 0;
    for (x = 0;  x < NSLC;  x++) {
	master_ob->interactive->slc[x][0] = slc_default_flags[x];
	master_ob->interactive->slc[x][1] = slc_default_chars[x];
    }
    all_users[i] = master_ob->interactive;
    all_users[i]->fd = new_socket_fd;
#ifdef F_QUERY_IP_PORT
    all_users[i]->local_port = external_port[which].port;
#endif
#ifdef F_NETWORK_STATS
    all_users[i]->external_port = which;
#endif
    set_prompt("> ");
    
    memcpy((char *) &all_users[i]->addr, (char *) &addr, length);
    debug(connections, ("New connection from %s.\n", inet_ntoa(addr.sin_addr)));
    num_user++;
    /*
     * The user object has one extra reference. It is asserted that the
     * master_ob is loaded.  Save a pointer to the master ob incase it
     * changes during APPLY_CONNECT.  We want to free the reference on
     * the right copy of the object.
     */
    master = master_ob;
    add_ref(master_ob, "new_user");
    push_number(external_port[which].port);
    ret = apply_master_ob(APPLY_CONNECT, 1);
    /* master_ob->interactive can be zero if the master object self
       destructed in the above (don't ask) */
    set_command_giver(0);
    if (ret == 0 || ret == (svalue_t *)-1 || ret->type != T_OBJECT
	|| !master_ob->interactive) {
	if (master_ob->interactive)
	    remove_interactive(master_ob, 0);
	else
	    free_object(master, "new_user");
	debug_message("Connection from %s aborted.\n", inet_ntoa(addr.sin_addr));
	return;
    }
    /*
     * There was an object returned from connect(). Use this as the user
     * object.
     */
    free_object(master, "new_user");
    ob = ret->u.ob;
#ifdef F_SET_HIDE
    if (ob->flags & O_HIDDEN)
	num_hidden_users++;
#endif
    ob->interactive = master_ob->interactive;
    ob->interactive->ob = ob;
    ob->flags |= O_ONCE_INTERACTIVE;
    /*
     * assume the existance of write_prompt and process_input in user.c
     * until proven wrong (after trying to call them).
     */
    ob->interactive->iflags |= (HAS_WRITE_PROMPT | HAS_PROCESS_INPUT);
    
    master_ob->flags &= ~O_ONCE_INTERACTIVE;
    master_ob->interactive = 0;
    add_ref(ob, "new_user");
    set_command_giver(ob);
    if (addr_server_fd >= 0) {
	query_addr_name(ob);
    }
    
    if (external_port[which].kind == PORT_TELNET) {
	/* Ask permission to ask them for their terminal type */
	add_binary_message(ob, telnet_do_ttype, sizeof(telnet_do_ttype));
	/* Ask them for their window size */
	add_binary_message(ob, telnet_do_naws, sizeof(telnet_do_naws));
    }
    
    logon(ob);
    debug(connections, ("new_user_handler: end\n"));
    set_command_giver(0);
}				/* new_user_handler() */

/*
 * Return the first command of the next user in sequence that has a complete
 * command in their buffer.  A command is defined to be a single character
 * when SINGLE_CHAR is set, or a newline terminated string otherwise.
 */
static char *get_user_command()
{
    static int NextCmdGiver = 0;

    int i;
    interactive_t *ip;
    char *user_command = 0;

    /* find and return a user command */
    for (i = 0; i < max_users; i++) {
	ip = all_users[NextCmdGiver++];
	NextCmdGiver %= max_users;

	if (!ip || !ip->ob || ip->ob->flags & O_DESTRUCTED)
	    continue;

	/* if we've got text to send, try to flush it, could lose the link here */
	if (ip->message_length) {
	    object_t *ob = ip->ob;
	    flush_message(ip);
	    if (!IP_VALID(ip, ob))
		continue;
	}

	/* if there's a command in the buffer, pull it out! */
	if (ip->iflags & CMD_IN_BUF) {
	    NextCmdGiver++;
	    NextCmdGiver %= max_users;
	    user_command = first_cmd_in_buf(ip);
	    break;
	}
    }

    /* no command found - return 0 */
    if (!user_command)
	return 0;

    /* got a command - return it and set command_giver */
    debug(connections, ("get_user_command: user_command = (%s)\n", user_command));
    save_command_giver(ip->ob);

#ifndef GET_CHAR_IS_BUFFERED
    if (ip->iflags & NOECHO) {
#else
    if ((ip->iflags & NOECHO) && !(ip->iflags & SINGLE_CHAR)) {
#endif
	/* must not enable echo before the user input is received */
	add_binary_message(command_giver, telnet_no_echo, sizeof(telnet_no_echo));
	ip->iflags &= ~NOECHO;
    }

    ip->last_time = current_time;
    return user_command;
}				/* get_user_command() */

static int escape_command P2(interactive_t *, ip, char *, user_command)
{
    if (user_command[0] != '!')
	return 0;
#ifdef OLD_ED
    if (ip->ed_buffer)
	return 1;
#endif
#if defined(F_INPUT_TO) || defined(F_GET_CHAR)
    if (ip->input_to && !(ip->iflags & NOESC))
	return 1;
#endif
    return 0;
}

static void process_input P2(interactive_t *, ip, char *, user_command)
{
    svalue_t *ret;

    if (!(ip->iflags & HAS_PROCESS_INPUT)) {
	parse_command(user_command, command_giver);
	return;
    }

    /*
     * send a copy of user input back to user object to provide
     * support for things like command history and mud shell
     * programming languages.
     */
    copy_and_push_string(user_command);
    ret = apply(APPLY_PROCESS_INPUT, command_giver, 1, ORIGIN_DRIVER);
    if (!IP_VALID(ip, command_giver))
	return;
    if (!ret) {
	ip->iflags &= ~HAS_PROCESS_INPUT;
	parse_command(user_command, command_giver);
	return;
    }

#ifndef NO_ADD_ACTION
    if (ret->type == T_STRING) {
	static char buf[MAX_TEXT];

	strncpy(buf, ret->u.string, MAX_TEXT - 1);
	parse_command(buf, command_giver);
    } else {
    	if (ret->type != T_NUMBER || !ret->u.number)
	    parse_command(user_command, command_giver);
    }
#endif
}

/*
 * This is the user command handler. This function is called when
 * a user command needs to be processed.
 * This function calls get_user_command() to get a user command.
 * One user command is processed per execution of this function.
 */
int process_user_command()
{
    char *user_command;
    interactive_t *ip;

    /*
     * WARNING: get_user_command() sets command_giver via
     * save_command_giver(), but only when the return is non-zero!
     */
    if (!(user_command = get_user_command()))
	return 0;

    ip = command_giver->interactive;
    current_interactive = command_giver;    /* this is yuck phooey, sigh */
    clear_notify(ip->ob);
    update_load_av();
    debug(connections, ("process_user_command: command_giver = /%s\n", command_giver->name));

    if (escape_command(ip, user_command)) {
	if (ip->iflags & SINGLE_CHAR) {
	    /* only 1 char ... switch to line buffer mode */
	    ip->iflags |= WAS_SINGLE_CHAR;
	    ip->iflags &= ~SINGLE_CHAR;
#ifdef GET_CHAR_IS_BUFFERED
	    ip->text_start = ip->text_end = *ip->text = 0;
#endif
	    set_linemode(ip);
	} else {
	    if (ip->iflags & WAS_SINGLE_CHAR) {
		/* we now have a string ... switch back to char mode */
		ip->iflags &= ~WAS_SINGLE_CHAR;
		ip->iflags |= SINGLE_CHAR;
		set_charmode(ip);
		if (!IP_VALID(ip, command_giver)) {
		    goto exit;
		}
	    }

	    process_input(ip, user_command + 1);
	}

	goto exit;
    }

#ifdef OLD_ED
    if (ip->ed_buffer) {
	ed_cmd(user_command);
	goto exit;
    }
#endif

#if defined(F_INPUT_TO) || defined(F_GET_CHAR)
    if (call_function_interactive(ip, user_command)) {
	goto exit;
    }
#endif

    process_input(ip, user_command);

exit:
    /*
     * Print a prompt if user is still here.
     */
    if (IP_VALID(ip, command_giver))
	print_prompt(ip);

    current_interactive = 0;
    restore_command_giver();
    return 1;
}

#define HNAME_BUF_SIZE 200
/*
 * This is the hname input data handler. This function is called by the
 * master handler when data is pending on the hname socket (addr_server_fd).
 */

static void hname_handler()
{
    static char hname_buf[HNAME_BUF_SIZE];
    static int hname_buf_pos;
    int num_bytes;

    num_bytes = HNAME_BUF_SIZE - hname_buf_pos - 1; /* room for nul */
    num_bytes = OS_socket_read(addr_server_fd, hname_buf + hname_buf_pos, num_bytes);
    if (num_bytes <= 0) {
	if (num_bytes == -1) {
#ifdef EWOULDBLOCK
	    if (socket_errno == EWOULDBLOCK) {
		debug(connections, ("hname_handler: read on fd %d: Operation would block.\n",
			    addr_server_fd));
		return;
	    }
#endif
	    debug_message("hname_handler: read on fd %d\n", addr_server_fd);
	    socket_perror("hname_handler: read", 0);
	} else {
	    debug_message("hname_handler: closing address server connection.\n");
	}
	OS_socket_close(addr_server_fd);
	addr_server_fd = -1;
	return;
    }

    hname_buf_pos += num_bytes;
    hname_buf[hname_buf_pos] = 0;
    debug(connections, ("hname_handler: address server replies: %s", hname_buf));

    while (hname_buf_pos) {
	char *nl, *pp;
	    
	/* if there's no newline, there's more data to come */
	if (!(nl = strchr(hname_buf, '\n')))
	    break;
	*nl++ = 0;

	if ((pp = strchr(hname_buf, ' ')) != 0) {
	    *pp++ = 0;
	    got_addr_number(pp, hname_buf);

	    if (isdigit(hname_buf[0])) {
		unsigned long laddr;

		if ((laddr = inet_addr(hname_buf)) != INADDR_NONE) {
		    if (strcmp(pp, "0") != 0)
			add_ip_entry(laddr, pp);
		}
	    }
	}

	hname_buf_pos -= (nl - hname_buf);
	if (hname_buf_pos)
	    memmove(hname_buf, nl, hname_buf_pos + 1); /* be sure to get the nul */
    }
}

/*
 * Remove an interactive user immediately.
 */
void remove_interactive P2(object_t *, ob, int, dested)
{
    int idx;
    /* don't have to worry about this dangling, since this is the routine
     * that causes this to dangle elsewhere, and we are protected from
     * getting called recursively by CLOSING.  safe_apply() should be
     * used here, since once we start this process we can't back out,
     * so jumping out with an error would be bad.
     */
    interactive_t *ip = ob->interactive;
    
    if (!ip) return;
    
    if (ip->iflags & CLOSING) {
	if (!dested)
	    debug_message("Double call to remove_interactive()\n");
	return;
    }

    debug(connections, ("Closing connection from %s.\n",
			inet_ntoa(ip->addr.sin_addr)));

    flush_message(ip);
    ip->iflags |= CLOSING;

#ifdef OLD_ED
    if (ip->ed_buffer) {
 	save_ed_buffer(ob);
    }
#else
    if (ob->flags & O_IN_EDIT) {
	object_save_ed_buffer(ob);
	ob->flags &= ~O_IN_EDIT;
    }
#endif

    if (!dested) {
        /*
	 * auto-notification of net death
	 */
	save_command_giver(ob);
	safe_apply(APPLY_NET_DEAD, ob, 0, ORIGIN_DRIVER);
	restore_command_giver();
    }
    
#ifndef NO_SNOOP
    if (ip->snooped_by) {
	ip->snooped_by->flags &= ~O_SNOOP;
	ip->snooped_by = 0;
    }
#endif

    debug(connections, ("remove_interactive: closing fd %d\n", ip->fd));
    if (OS_socket_close(ip->fd) == -1) {
 	socket_perror("remove_interactive: close", 0);
    }
#ifdef F_SET_HIDE
    if (ob->flags & O_HIDDEN)
	num_hidden_users--;
#endif
    num_user--;
    clear_notify(ip->ob);
#if defined(F_INPUT_TO) || defined(F_GET_CHAR)
    if (ip->input_to) {
	free_object(ip->input_to->ob, "remove_interactive");
	free_sentence(ip->input_to);
	if (ip->num_carry > 0)
	    free_some_svalues(ip->carryover, ip->num_carry);
	ip->carryover = NULL;
	ip->num_carry = 0;
	ip->input_to = 0;
    }
#endif
    for (idx = 0; idx < max_users; idx++)
	if (all_users[idx] == ip) break;
    DEBUG_CHECK(idx == max_users, "remove_interactive: could not find and remove user!\n");
    FREE(ip);
    ob->interactive = 0;
    all_users[idx] = 0;
    free_object(ob, "remove_interactive");
    return;
}				/* remove_interactive() */

#if defined(F_INPUT_TO) || defined(F_GET_CHAR)
static int call_function_interactive P2(interactive_t *, i, char *, str)
{
    object_t *ob;
    funptr_t *funp;
    char *function;
    svalue_t *args;
    sentence_t *sent;
    int num_arg;
#ifdef GET_CHAR_IS_BUFFERED
    int was_single = 0;
    int was_noecho = 0;
#endif

    i->iflags &= ~NOESC;
    if (!(sent = i->input_to))
	return (0);

    /*
     * Special feature: input_to() has been called to setup a call to a
     * function.
     */
    if (sent->ob->flags & O_DESTRUCTED) {
	/* Sorry, the object has selfdestructed ! */
	free_object(sent->ob, "call_function_interactive");
	free_sentence(sent);
	i->input_to = 0;
	if (i->num_carry)
	    free_some_svalues(i->carryover, i->num_carry);
	i->carryover = NULL;
	i->num_carry = 0;
	return (0);
    }
    /*
     * We must all references to input_to fields before the call to apply(),
     * because someone might want to set up a new input_to().
     */
    free_object(sent->ob, "call_function_interactive");
    /* we put the function on the stack in case of an error */
    STACK_INC;
    if (sent->flags & V_FUNCTION) {
      function = 0;
      sp->type = T_FUNCTION;
      sp->u.fp = funp = sent->function.f;
      funp->hdr.ref++;
    } else {
      sp->type = T_STRING;
      sp->subtype = STRING_SHARED;
      sp->u.string = function = sent->function.s;
      ref_string(function);
    }
    ob = sent->ob;
    free_sentence(sent);

    /*
     * If we have args, we have to copy them, so the svalues on the
     * interactive struct can be FREEd
     */
    num_arg = i->num_carry;
    if (num_arg) {
	args = i->carryover;
	i->num_carry = 0;
	i->carryover = NULL;
    } else
	args = NULL;

    i->input_to = 0;
    if (i->iflags & SINGLE_CHAR) {
	/*
	 * clear single character mode
	 */
	i->iflags &= ~SINGLE_CHAR;
#ifndef GET_CHAR_IS_BUFFERED
	set_linemode(i);
#else
	was_single = 1;
	if (i->iflags & NOECHO) {
	    was_noecho = 1;
	    i->iflags &= ~NOECHO;
	}
#endif
    }

    copy_and_push_string(str);
    /*
     * If we have args, we have to push them onto the stack in the order they
     * were in when we got them.  They will be popped off by the called
     * function.
     */
    if (args) {
	transfer_push_some_svalues(args, num_arg);
	FREE(args);
    }
    /* current_object no longer set */
    if (function) {
	if (function[0] == APPLY___INIT_SPECIAL_CHAR)
	    error("Illegal function name.\n");
       (void) apply(function, ob, num_arg + 1, ORIGIN_INTERNAL);
    } else
       call_function_pointer(funp, num_arg + 1);

    pop_stack();		/* remove `function' from stack */

#ifdef GET_CHAR_IS_BUFFERED
    if (IP_VALID(i, ob)) {
	if (was_single && !(i->iflags & SINGLE_CHAR)) {
	    i->text_start = i->text_end = 0;
	    i->text[0] = '\0';
	    i->iflags &= ~CMD_IN_BUF;
	    set_linemode(i);
	}
	if (was_noecho && !(i->iflags & NOECHO))
	    add_binary_message(i->ob, telnet_no_echo, sizeof(telnet_no_echo));
    }
#endif

    return (1);
}				/* call_function_interactive() */

int set_call P3(object_t *, ob, sentence_t *, sent, int, flags)
{
    if (ob == 0 || sent == 0)
	return (0);
    if (ob->interactive == 0 || ob->interactive->input_to)
	return (0);
    ob->interactive->input_to = sent;
    ob->interactive->iflags |= (flags & (I_NOECHO | I_NOESC | I_SINGLE_CHAR));
    if (flags & I_NOECHO)
	add_binary_message(ob, telnet_yes_echo, sizeof(telnet_yes_echo));
    if (flags & I_SINGLE_CHAR)
	set_charmode(ob->interactive);
    return (1);
}				/* set_call() */
#endif

void set_prompt P1(char *, str)
{
    if (command_giver && command_giver->interactive) {
	command_giver->interactive->prompt = str;
    }
}				/* set_prompt() */

/*
 * Print the prompt, but only if input_to not is disabled.
 */
static void print_prompt P1(interactive_t*, ip)
{
    object_t *ob = ip->ob;
    
#if defined(F_INPUT_TO) || defined(F_GET_CHAR)
    if (ip->input_to == 0) {
#endif
	/* give user object a chance to write its own prompt */
	if (!(ip->iflags & HAS_WRITE_PROMPT))
	    tell_object(ip->ob, ip->prompt, strlen(ip->prompt));
#ifdef OLD_ED
	else if (ip->ed_buffer)
	    tell_object(ip->ob, ip->prompt, strlen(ip->prompt));
#endif
	else if (!apply(APPLY_WRITE_PROMPT, ip->ob, 0, ORIGIN_DRIVER)) {
	    if (!IP_VALID(ip, ob)) return;
	    ip->iflags &= ~HAS_WRITE_PROMPT;
	    tell_object(ip->ob, ip->prompt, strlen(ip->prompt));
	}
#if defined(F_INPUT_TO) || defined(F_GET_CHAR)
    }
#endif
    if (!IP_VALID(ip, ob)) return;
    /*
     * Put the IAC GA thing in here... Moved from before writing the prompt;
     * vt src says it's a terminator. Should it be inside the no-input_to
     * case? We'll see, I guess.
     */
    if ((ip->iflags & USING_TELNET) && !(ip->iflags & SUPPRESS_GA))
	add_binary_message(command_giver, telnet_ga, sizeof(telnet_ga));
    if (!IP_VALID(ip, ob)) return;
}				/* print_prompt() */

/*
 * Let object 'me' snoop object 'you'. If 'you' is 0, then turn off
 * snooping.
 *
 * This routine is almost identical to the old set_snoop. The main
 * difference is that the routine writes nothing to user directly,
 * all such communication is taken care of by the mudlib. It communicates
 * with master.c in order to find out if the operation is permissble or
 * not. The old routine let everyone snoop anyone. This routine also returns
 * 0 or 1 depending on success.
 */
#ifndef NO_SNOOP
int new_set_snoop P2(object_t *, by, object_t *, victim)
{
    interactive_t *ip;
    object_t *tmp;
    
    if (by->flags & O_DESTRUCTED)
	return 0;
    if (victim && (victim->flags & O_DESTRUCTED))
	return 0;

    if (victim) {
	if (!victim->interactive)
	    error("Second argument of snoop() is not interactive!\n");
	ip = victim->interactive;
    } else {
	/*
	 * Stop snoop.
	 */
	if (by->flags & O_SNOOP) {
	    int i;
	    
	    for (i = 0; i < max_users; i++) {
		if (all_users[i] && all_users[i]->snooped_by == by)
		    all_users[i]->snooped_by = 0;
	    }
	    by->flags &= ~O_SNOOP;
	}
	return 1;
    }

    /*
     * Protect against snooping loops.
     */
    tmp = by;
    while (tmp) {
	if (tmp == victim)
	    return 0;

	/* the person snooping us, if any */
	tmp = (tmp->interactive ? tmp->interactive->snooped_by : 0);
    }
    
    /*
     * Terminate previous snoop, if any.
     */
    if (by->flags & O_SNOOP) {
	int i;
	
	for (i = 0; i < max_users; i++) {
	    if (all_users[i] && all_users[i]->snooped_by == by)
		all_users[i]->snooped_by = 0;
	}
    }
    if (ip->snooped_by)
	ip->snooped_by->flags &= ~O_SNOOP;
    by->flags |= O_SNOOP;
    ip->snooped_by = by;

    return 1;
}				/* set_new_snoop() */
#endif

static void query_addr_name P1(object_t *, ob)
{
    static char buf[100];
    static char *dbuf = &buf[sizeof(int) + sizeof(int) + sizeof(int)];
    int msglen;
    int msgtype;

    sprintf(dbuf, "%s", query_ip_number(ob));
    msglen = sizeof(int) + strlen(dbuf) +1;

    msgtype = DATALEN;
    memcpy(buf, (char *) &msgtype, sizeof(msgtype));
    memcpy(&buf[sizeof(int)], (char *) &msglen, sizeof(msglen));

    msgtype = NAMEBYIP;
    memcpy(&buf[sizeof(int) + sizeof(int)], (char *) &msgtype, sizeof(msgtype));
    debug(connections, ("query_addr_name: sent address server %s\n", dbuf));

    if (OS_socket_write(addr_server_fd, buf, msglen + sizeof(int) + sizeof(int)) == -1) {
	switch (socket_errno) {
	case EBADF:
	    debug_message("Address server has closed connection.\n");
	    addr_server_fd = -1;
	    break;
	default:
	    socket_perror("query_addr_name: write", 0);
	    break;
	}
    }
}				/* query_addr_name() */

#define IPSIZE 200
typedef struct {
    char *name;
    svalue_t call_back;
    object_t *ob_to_call;
} ipnumberentry_t;

static ipnumberentry_t ipnumbertable[IPSIZE];

/*
 * Does a call back on the current_object with the function call_back.
 */
int query_addr_number P2(char *, name, svalue_t *, call_back)
{
    static char buf[100];
    static char *dbuf = &buf[sizeof(int) + sizeof(int) + sizeof(int)];
    int msglen;
    int msgtype;

    if ((addr_server_fd < 0) || (strlen(name) >=
		  100 - (sizeof(msgtype) + sizeof(msglen) + sizeof(int)))) {
	share_and_push_string(name);
	push_undefined();
	if (call_back->type == T_STRING)
	    apply(call_back->u.string, current_object, 2, ORIGIN_INTERNAL);
	else
	    call_function_pointer(call_back->u.fp, 2);
	return 0;
    }
    strcpy(dbuf, name);
    msglen = sizeof(int) + strlen(name) +1;

    msgtype = DATALEN;
    memcpy(buf, (char *) &msgtype, sizeof(msgtype));
    memcpy(&buf[sizeof(int)], (char *) &msglen, sizeof(msglen));

    msgtype = (name[0] >= '0' && name[0] <= '9') ? NAMEBYIP : IPBYNAME;
    memcpy(&buf[sizeof(int) + sizeof(int)], (char *) &msgtype, sizeof(msgtype));

    debug(connections, ("query_addr_number: sent address server %s\n", dbuf));

    if (OS_socket_write(addr_server_fd, buf, msglen + sizeof(int) + sizeof(int)) == -1) {
	switch (socket_errno) {
	case EBADF:
	    debug_message("Address server has closed connection.\n");
	    addr_server_fd = -1;
	    break;
	default:
	    socket_perror("query_addr_name: write", 0);
	    break;
	}
	share_and_push_string(name);
	push_undefined();
	if (call_back->type == T_STRING)
	    apply(call_back->u.string, current_object, 2, ORIGIN_INTERNAL);
	else
	    call_function_pointer(call_back->u.fp, 2);
	return 0;
    } else {
	int i;

/* We put ourselves into the pending name lookup entry table */
/* Find the first free entry */
	for (i = 0; i < IPSIZE && ipnumbertable[i].name; i++)
	    ;
	if (i == IPSIZE) {
/* We need to error...  */
	    share_and_push_string(name);
	    push_undefined();
	    if (call_back->type == T_STRING)
		apply(call_back->u.string, current_object, 2, ORIGIN_INTERNAL);
	    else
		call_function_pointer(call_back->u.fp, 2);
	    return 0;
	}
/* Create our entry... */
	ipnumbertable[i].name = make_shared_string(name);
	assign_svalue_no_free(&ipnumbertable[i].call_back, call_back);
	ipnumbertable[i].ob_to_call = current_object;
	add_ref(current_object, "query_addr_number: ");
	return i + 1;
    }
}				/* query_addr_number() */

static void got_addr_number P2(char *, number, char *, name)
{
    int i;
    char *theName, *theNumber;

    /* First remove all the dested ones... */
    for (i = 0; i < IPSIZE; i++)
	if (ipnumbertable[i].name
	    && ipnumbertable[i].ob_to_call->flags & O_DESTRUCTED) {
	    free_svalue(&ipnumbertable[i].call_back, "got_addr_number");
	    free_string(ipnumbertable[i].name);
	    free_object(ipnumbertable[i].ob_to_call, "got_addr_number: ");
	    ipnumbertable[i].name = NULL;
	}
    for (i = 0; i < IPSIZE; i++) {
	if (ipnumbertable[i].name && strcmp(name, ipnumbertable[i].name)== 0) {
	    /* Found one, do the call back... */
	    theName = ipnumbertable[i].name;
	    theNumber = number;
	    
	    if (uisdigit(theName[0])) {
		char *tmp;
		
		tmp = theName;
		theName = theNumber;
		theNumber = tmp;
	    }
	    if (strcmp(theName, "0")) {
		share_and_push_string(theName);
	    } else {
		push_undefined();
	    }
	    if (strcmp(theNumber, "0")) {
		share_and_push_string(theNumber);
	    } else {
		push_undefined();
	    }
	    push_number(i + 1);
	    if (ipnumbertable[i].call_back.type == T_STRING)
		safe_apply(ipnumbertable[i].call_back.u.string, 
			   ipnumbertable[i].ob_to_call,
			   3, ORIGIN_INTERNAL);
	    else
		safe_call_function_pointer(ipnumbertable[i].call_back.u.fp, 3);
	    free_svalue(&ipnumbertable[i].call_back, "got_addr_number");
	    free_string(ipnumbertable[i].name);
	    free_object(ipnumbertable[i].ob_to_call, "got_addr_number: ");
	    ipnumbertable[i].name = NULL;
	}
    }
}				/* got_addr_number() */

#undef IPSIZE
#define IPSIZE 200
typedef struct {
    long addr;
    char *name;
} ipentry_t;

static ipentry_t iptable[IPSIZE];
static int ipcur;

#ifdef DEBUGMALLOC_EXTENSIONS
void mark_iptable() {
    int i;

    for (i=0; i < IPSIZE; i++)
	if (iptable[i].name)
	    EXTRA_REF(BLOCK(iptable[i].name))++;
}
#endif

char *query_ip_name P1(object_t *, ob)
{
    int i;

    if (ob == 0)
	ob = command_giver;
    if (!ob || ob->interactive == 0)
	return NULL;
    for (i = 0; i < IPSIZE; i++) {
	if (iptable[i].addr == ob->interactive->addr.sin_addr.s_addr &&
	    iptable[i].name)
	    return (iptable[i].name);
    }
    return (inet_ntoa(ob->interactive->addr.sin_addr));
}

static void add_ip_entry P2(long, addr, char *, name)
{
    int i;

    for (i = 0; i < IPSIZE; i++) {
	if (iptable[i].addr == addr)
	    return;
    }
    iptable[ipcur].addr = addr;
    if (iptable[ipcur].name)
	free_string(iptable[ipcur].name);
    iptable[ipcur].name = make_shared_string(name);
    ipcur = (ipcur + 1) % IPSIZE;
}

char *query_ip_number P1(object_t *, ob)
{
    if (ob == 0)
	ob = command_giver;
    if (!ob || ob->interactive == 0)
	return 0;
    return (inet_ntoa(ob->interactive->addr.sin_addr));
}

#ifndef INET_NTOA_OK
/*
 * Note: if the address string is "a.b.c.d" the address number is
 *       a * 256^3 + b * 256^2 + c * 256 + d
 */
char *inet_ntoa P1(struct in_addr, ad)
{
    u_long s_ad;
    int a, b, c, d;
    static char addr[20];	/* 16 + 1 should be enough */

    s_ad = ad.s_addr;
    d = s_ad % 256;
    s_ad /= 256;
    c = s_ad % 256;
    s_ad /= 256;
    b = s_ad % 256;
    a = s_ad / 256;
    sprintf(addr, "%d.%d.%d.%d", a, b, c, d);
    return (addr);
}
#endif				/* INET_NTOA_OK */

char *query_host_name()
{
    static char name[40];

    gethostname(name, sizeof(name));
    name[sizeof(name) - 1] = '\0';	/* Just to make sure */
    return (name);
}				/* query_host_name() */

#ifndef NO_SNOOP
object_t *query_snoop P1(object_t *, ob)
{
    if (!ob->interactive)
	return 0;
    return ob->interactive->snooped_by;
}				/* query_snoop() */

object_t *query_snooping P1(object_t *, ob)
{
    int i;
    
    if (!(ob->flags & O_SNOOP)) return 0;
    for (i = 0; i < max_users; i++) {
	if (all_users[i] && all_users[i]->snooped_by == ob)
	    return all_users[i]->ob;
    }
    fatal("couldn't find snoop target.\n");
    return 0;
}				/* query_snooping() */
#endif

int query_idle P1(object_t *, ob)
{
    if (!ob->interactive)
	error("query_idle() of non-interactive object.\n");
    return (current_time - ob->interactive->last_time);
}				/* query_idle() */

#ifdef F_EXEC
int replace_interactive P2(object_t *, ob, object_t *, obfrom)
{
    if (ob->interactive) {
	error("Bad argument 1 to exec()\n");
    }
    if (!obfrom->interactive) {
	error("Bad argument 2 to exec()\n");
    }
#ifdef F_SET_HIDE
    if ((ob->flags & O_HIDDEN) != (obfrom->flags & O_HIDDEN)) {
	if (ob->flags & O_HIDDEN) {
	    num_hidden_users++;
	} else {
	    num_hidden_users--;
	}
    }
#endif
    ob->interactive = obfrom->interactive;
    /*
     * assume the existance of write_prompt and process_input in user.c until
     * proven wrong (after trying to call them).
     */
    ob->interactive->iflags |= (HAS_WRITE_PROMPT | HAS_PROCESS_INPUT);
    obfrom->interactive = 0;
    ob->interactive->ob = ob;
    ob->flags |= O_ONCE_INTERACTIVE;
    obfrom->flags &= ~O_ONCE_INTERACTIVE;
    add_ref(ob, "exec");
    free_object(obfrom, "exec");
    if (obfrom == command_giver) {
	set_command_giver(ob);
    }
    return (1);
}				/* replace_interactive() */
#endif

void outbuf_zero P1(outbuffer_t *, outbuf) {
    outbuf->real_size = 0;
    outbuf->buffer = 0;
}

int outbuf_extend P2(outbuffer_t *, outbuf, int, l)
{
    int limit;

    DEBUG_CHECK(l < 0, "Negative length passed to outbuf_extend.\n");
    l = (l > USHRT_MAX ? USHRT_MAX : l);
    
    if (outbuf->buffer) {
	limit = MSTR_SIZE(outbuf->buffer);
	if (outbuf->real_size + l > limit) {
	    if (outbuf->real_size == USHRT_MAX) return 0; /* TRUNCATED */

	    /* assume it's going to grow some more */
	    limit = (outbuf->real_size + l) * 2;
	    if (limit > USHRT_MAX) {
		limit = USHRT_MAX;
		outbuf->buffer = extend_string(outbuf->buffer, USHRT_MAX);
		return USHRT_MAX - outbuf->real_size;
	    }
	    outbuf->buffer = extend_string(outbuf->buffer, limit);
	}
    } else {
	outbuf->buffer = new_string(l, "outbuf_extend");
	outbuf->real_size = 0;
    }
    return l;
}

void outbuf_add P2(outbuffer_t *, outbuf, char *, str)
{
    int l, limit;
    
    if (!outbuf) return;
    l = strlen(str);
    if ((limit = outbuf_extend(outbuf, l)) > 0) {
	strncpy(outbuf->buffer + outbuf->real_size, str, limit);
	outbuf->real_size += (l > limit ? limit : l);
	*(outbuf->buffer + outbuf->real_size) = 0;
    }
}

void outbuf_addchar P2(outbuffer_t *, outbuf, char, c)
{
    if (outbuf && (outbuf_extend(outbuf, 1)) > 0) {
        *(outbuf->buffer + outbuf->real_size++) = c;
	*(outbuf->buffer + outbuf->real_size) = 0;
    }
}

void outbuf_addv P2V(outbuffer_t *, outbuf, char *, format)
{
    char buf[LARGEST_PRINTABLE_STRING + 1];
    va_list args;
    V_DCL(char *format);
    V_DCL(outbuffer_t *outbuf);

    V_START(args, format);
    V_VAR(outbuffer_t *, outbuf, args);
    V_VAR(char *, format, args);

    vsprintf(buf, format, args);
    va_end(args);

    if (!outbuf) return;
    
    outbuf_add(outbuf, buf);
}

void outbuf_fix P1(outbuffer_t *, outbuf) {
    if (outbuf && outbuf->buffer)
	outbuf->buffer = extend_string(outbuf->buffer, outbuf->real_size);
}

void outbuf_push P1(outbuffer_t *, outbuf) {
    STACK_INC;
    sp->type = T_STRING;
    if (outbuf && outbuf->buffer) {
	outbuf->buffer = extend_string(outbuf->buffer, outbuf->real_size);
	
	sp->subtype = STRING_MALLOC;
	sp->u.string = outbuf->buffer;
    } else {
	sp->subtype = STRING_CONSTANT;
	sp->u.string = "";
    }
}
