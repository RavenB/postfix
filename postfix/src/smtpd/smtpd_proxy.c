/*++
/* NAME
/*	smtpd_proxy 3
/* SUMMARY
/*	SMTP server pass-through proxy client
/* SYNOPSIS
/*	#include <smtpd.h>
/*	#include <smtpd_proxy.h>
/*
/*	typedef struct {
/* .in +4
/*		/* other fields... */
/*		VSTREAM *proxy;		/* connection to SMTP proxy */
/*		VSTRING *proxy_buffer;	/* last SMTP proxy response */
/*		/* other fields... */
/* .in -4
/*	} SMTPD_STATE;
/*
/*	int	smtpd_proxy_open(state, service, timeout, ehlo_name, mail_from)
/*	SMTPD_STATE *state;
/*	const char *service;
/*	int	timeout;
/*	const char *ehlo_name;
/*	const char *mail_from;
/*
/*	int	smtpd_proxy_cmd(state, expect, format, ...)
/*	SMTPD_STATE *state;
/*	int	expect;
/*	cont char *format;
/*
/*	void	smtpd_proxy_close(state)
/*	SMTPD_STATE *state;
/* RECORD-LEVEL ROUTINES
/*	int	smtpd_proxy_rec_put(stream, rec_type, data, len)
/*	VSTREAM *stream;
/*	int	rec_type;
/*	const char *data;
/*	int	len;
/*
/*	int	smtpd_proxy_rec_fprintf(stream, rec_type, format, ...)
/*	VSTREAM *stream;
/*	int	rec_type;
/*	cont char *format;
/* DESCRIPTION
/*	The functions in this module implement a pass-through proxy
/*	client.
/*
/*	In order to minimize the intrusiveness of pass-through proxying, 1) the
/*	proxy server must support the same MAIL FROM/RCPT syntax that Postfix
/*	supports, 2) the record-level routines for message content proxying
/*	have the same interface as the routines that are used for non-proxied
/*	mail.
/*
/*	smtpd_proxy_open() connects to the proxy service, sends EHLO, sends
/*	client information with the XCLIENT command if possible, sends
/*	the MAIL FROM command, and receives the reply. A non-zero result means
/*	trouble: either the proxy is unavailable, or it did not send the
/*	expected reply.
/*	The result is reported via the state->proxy_buffer field in a form
/*	that can be sent to the SMTP client. In case of error, the
/*	state->error_mask and state->err fields are updated.
/*	A state->proxy_buffer field is created automatically; this field
/*	persists beyond the end of a proxy session.
/*
/*	smtpd_proxy_cmd() formats and sends the specified command to the
/*	proxy server, and receives the proxy server reply. A non-zero result
/*	means trouble: either the proxy is unavailable, or it did not send the
/*      expected reply.
/*	All results are reported via the state->proxy_buffer field in a form
/*	that can be sent to the SMTP client. In case of error, the
/*	state->error_mask and state->err fields are updated.
/*
/*	smtpd_proxy_close() disconnects from a proxy server and resets
/*	the state->proxy field. The last proxy server reply or error
/*	description remains available via state->proxy-reply.
/*
/*	smtpd_proxy_rec_put() is a rec_put() clone that passes arbitrary
/*	message content records to the proxy server. The data is expected
/*	to be in SMTP dot-escaped form. All errors are reported as a
/*	REC_TYPE_ERROR result value.
/*
/*	smtpd_proxy_rec_fprintf() is a rec_fprintf() clone that formats
/*	message content and sends it to the proxy server. Leading dots are
/*	not escaped. All errors are reported as a REC_TYPE_ERROR result
/*	value.
/*
/* Arguments:
/* .IP server
/*	The SMTP proxy server host:port. The host or host: part is optional.
/* .IP timeout
/*	Time limit for connecting to the proxy server and for
/*	sending and receiving proxy server commands and replies.
/* .IP ehlo_name
/*	The EHLO Hostname that will be sent to the proxy server.
/* .IP mail_from
/*	The MAIL FROM command.
/* .IP state
/*	SMTP server state.
/* .IP expect
/*	Expected proxy server reply status code range. A warning is logged
/*	when an unexpected reply is received. Specify one of the following:
/* .RS
/* .IP SMTPD_PROX_WANT_OK
/*	The caller expects a reply in the 200 range.
/* .IP SMTPD_PROX_WANT_MORE
/*	The caller expects a reply in the 300 range.
/* .IP SMTPD_PROX_WANT_ANY
/*	The caller has no expectation. Do not warn for unexpected replies.
/* .IP SMTPD_PROX_WANT_NONE
/*	Do not bother waiting for a reply.
/* .RE
/* .IP format
/*	A format string.
/* .IP stream
/*	Connection to proxy server.
/* .IP data
/*	Pointer to the content of one message content record.
/* .IP len
/*	The length of a message content record.
/* SEE ALSO
/*	smtpd(8) Postfix smtp server
/* DIAGNOSTICS
/*	Fatal errors: memory allocation problem.
/*
/*	Warnings: unexpected response from proxy server, unable
/*	to connect to proxy server, proxy server read/write error.
/* LICENSE
/* .ad
/* .fi
/*	The Secure Mailer license must be distributed with this software.
/* AUTHOR(S)
/*	Wietse Venema
/*	IBM T.J. Watson Research
/*	P.O. Box 704
/*	Yorktown Heights, NY 10598, USA
/*--*/

/* System library. */

#include <sys_defs.h>
#include <ctype.h>

/* Utility library. */

#include <msg.h>
#include <vstream.h>
#include <vstring.h>
#include <stringops.h>
#include <connect.h>

/* Global library. */

#include <mail_error.h>
#include <smtp_stream.h>
#include <cleanup_user.h>
#include <mail_params.h>
#include <rec_type.h>
#include <xtext.h>
#include <mail_proto.h>

/* Application-specific. */

#include <smtpd.h>
#include <smtpd_proxy.h>

 /*
  * SLMs.
  */
#define STR(x)	vstring_str(x)
#define LEN(x)	VSTRING_LEN(x)
#define SMTPD_PROXY_CONNECT ((char *) 0)

/* smtpd_proxy_open - open proxy connection */

int     smtpd_proxy_open(SMTPD_STATE *state, const char *service,
			         int timeout, const char *ehlo_name,
			         const char *mail_from)
{
    int     fd;
    char   *lines;
    char   *line;
    VSTRING *buf;
    int     bad;

    /*
     * This buffer persists beyond the end of a proxy session so we can
     * inspect the last command's reply.
     */
    if (state->proxy_buffer == 0)
	state->proxy_buffer = vstring_alloc(10);

    /*
     * Connect to proxy.
     */
    if ((fd = inet_connect(service, BLOCKING, timeout)) < 0) {
	state->error_mask |= MAIL_ERROR_SOFTWARE;
	state->err |= CLEANUP_STAT_PROXY;
	msg_warn("connect to proxy service %s: %m", service);
	vstring_sprintf(state->proxy_buffer,
			"451 Error: queue file write error");
	return (-1);
    }
    state->proxy = vstream_fdopen(fd, O_RDWR);
    vstream_control(state->proxy, VSTREAM_CTL_PATH, service, VSTREAM_CTL_END);
    smtp_timeout_setup(state->proxy, timeout);

    /*
     * Get server greeting banner.
     * 
     * If this fails then we have a problem because the proxy should always
     * accept our connection. Make up our own response instead of passing
     * back the greeting banner: the proxy open might be delayed to the point
     * that the client expects a MAIL FROM or RCPT TO reply.
     */
    if (smtpd_proxy_cmd(state, SMTPD_PROX_WANT_OK, SMTPD_PROXY_CONNECT) != 0) {
	vstring_sprintf(state->proxy_buffer,
			"451 Error: queue file write error");
	smtpd_proxy_close(state);
	return (-1);
    }

    /*
     * Send our own EHLO command. If this fails then we have a problem
     * because the proxy should always accept our EHLO command. Make up our
     * own response instead of passing back the EHLO reply: the proxy open
     * might be delayed to the point that the client expects a MAIL FROM or
     * RCPT TO reply.
     */
    if (smtpd_proxy_cmd(state, SMTPD_PROX_WANT_OK, "EHLO %s", ehlo_name) != 0) {
	vstring_sprintf(state->proxy_buffer,
			"451 Error: queue file write error");
	smtpd_proxy_close(state);
	return (-1);
    }

    /*
     * Parse the EHLO reply and see if we can forward logging information.
     */
    state->proxy_features = 0;
    lines = STR(state->proxy_buffer);
    while ((line = mystrtok(&lines, "\n")) != 0)
	if (ISDIGIT(line[0]) && ISDIGIT(line[1]) && ISDIGIT(line[2])
	    && (line[3] == ' ' || line[3] == '-')
	    && strcmp(line + 4, XCLIENT_CMD) == 0)
	    state->proxy_features |= SMTPD_FEATURE_XCLIENT;

    /*
     * Send all XCLIENT attributes. Transform internal forms to external
     * forms and encode the result as xtext.
     */
    if (state->proxy_features & SMTPD_FEATURE_XCLIENT) {
	buf = vstring_alloc(100);
	vstring_strcpy(buf, XCLIENT_CMD " " XCLIENT_FORWARD
		       " " XCLIENT_NAME "=");
	if (!IS_UNK_CLNT_NAME(FORWARD_NAME(state)))
	    xtext_quote_append(buf, FORWARD_NAME(state), "");
	vstring_strcat(buf, " " XCLIENT_ADDR "=");
	if (!IS_UNK_CLNT_ADDR(FORWARD_ADDR(state)))
	    xtext_quote_append(buf, FORWARD_ADDR(state), "");
	bad = smtpd_proxy_cmd(state, SMTPD_PROX_WANT_ANY, "%s", STR(buf));
	if (bad == 0) {
	    vstring_strcpy(buf, XCLIENT_CMD " " XCLIENT_FORWARD
			   " " XCLIENT_HELO "=");
	    if (!IS_UNK_HELO_NAME(FORWARD_HELO(state)))
		xtext_quote_append(buf, FORWARD_HELO(state), "");
	    vstring_strcat(buf, " " XCLIENT_PROTO "=");
	    if (!IS_UNK_PROTOCOL(FORWARD_PROTO(state)))
		xtext_quote_append(buf, FORWARD_PROTO(state), "");
	    bad = smtpd_proxy_cmd(state, SMTPD_PROX_WANT_ANY, "%s", STR(buf));
	}
	vstring_free(buf);
	if (bad) {
	    vstring_sprintf(state->proxy_buffer,
			    "451 Error: queue file write error");
	    smtpd_proxy_close(state);
	    return (-1);
	}
    }

    /*
     * Pass-through the client's MAIL FROM command. If this fails, then we
     * have a problem because the proxy should always accept any MAIL FROM
     * command that was accepted by us.
     */
    if (smtpd_proxy_cmd(state, SMTPD_PROX_WANT_OK, "%s", mail_from) != 0) {
	smtpd_proxy_close(state);
	return (-1);
    }
    return (0);
}

/* smtpd_proxy_rdwr_error - report proxy communication error */

static int smtpd_proxy_rdwr_error(VSTREAM *stream, int err)
{
    switch (err) {
	case SMTP_ERR_EOF:
	msg_warn("lost connection with proxy %s", VSTREAM_PATH(stream));
	return (err);
    case SMTP_ERR_TIME:
	msg_warn("timeout talking to proxy %s", VSTREAM_PATH(stream));
	return (err);
    default:
	msg_panic("smtpd_proxy_rdwr_error: unknown proxy %s stream error %d",
		  VSTREAM_PATH(stream), err);
    }
}

/* smtpd_proxy_cmd_error - report unexpected proxy reply */

static void smtpd_proxy_cmd_error(SMTPD_STATE *state, const char *fmt,
				          va_list ap)
{
    VSTRING *buf;

    /*
     * The command can be omitted at the start of an SMTP session. A null
     * format string is not documented as part of the official interface
     * because it is used only internally to this module.
     */
    buf = vstring_alloc(100);
    vstring_vsprintf(buf, fmt == SMTPD_PROXY_CONNECT ?
		     "connection request" : fmt, ap);
    msg_warn("proxy %s rejected \"%s\": \"%s\"", VSTREAM_PATH(state->proxy),
	     STR(buf), STR(state->proxy_buffer));
    vstring_free(buf);
}

/* smtpd_proxy_cmd - send command to proxy, receive reply */

int     smtpd_proxy_cmd(SMTPD_STATE *state, int expect, const char *fmt,...)
{
    va_list ap;
    char   *cp;
    int     last_char;
    int     err = 0;
    static VSTRING *buffer = 0;

    /*
     * Errors first. Be prepared for delayed errors from the DATA phase.
     */
    if (vstream_ftimeout(state->proxy)
	|| vstream_ferror(state->proxy)
	|| vstream_feof(state->proxy)
	|| ((err = vstream_setjmp(state->proxy) != 0)
	    && smtpd_proxy_rdwr_error(state->proxy, err))) {
	state->error_mask |= MAIL_ERROR_SOFTWARE;
	state->err |= CLEANUP_STAT_PROXY;
	vstring_sprintf(state->proxy_buffer,
			"451 Error: queue file write error");
	return (-1);
    }

    /*
     * The command can be omitted at the start of an SMTP session. This is
     * not documented as part of the official interface because it is used
     * only internally to this module.
     */
    if (fmt != SMTPD_PROXY_CONNECT) {

	/*
	 * Format the command.
	 */
	va_start(ap, fmt);
	vstring_vsprintf(state->proxy_buffer, fmt, ap);
	va_end(ap);

	/*
	 * Optionally log the command first, so that we can see in the log
	 * what the program is trying to do.
	 */
	if (msg_verbose)
	    msg_info("> %s: %s", VSTREAM_PATH(state->proxy),
		     STR(state->proxy_buffer));

	/*
	 * Send the command to the proxy server. Since we're going to read a
	 * reply immediately, there is no need to flush buffers.
	 */
	smtp_fputs(STR(state->proxy_buffer), LEN(state->proxy_buffer),
		   state->proxy);
    }

    /*
     * Early return if we don't want to wait for a server reply (such as
     * after sending QUIT.
     */
    if (expect == SMTPD_PROX_WANT_NONE)
	return (0);

    /*
     * Censor out non-printable characters in server responses and save
     * complete multi-line responses if possible.
     */
    VSTRING_RESET(state->proxy_buffer);
    if (buffer == 0)
	buffer = vstring_alloc(10);
    for (;;) {
	last_char = smtp_get(buffer, state->proxy, var_line_limit);
	printable(STR(buffer), '?');
	if (last_char != '\n')
	    msg_warn("%s: response longer than %d: %.30s...",
		     VSTREAM_PATH(state->proxy), var_line_limit,
		     STR(buffer));
	if (msg_verbose)
	    msg_info("< %s: %.100s", VSTREAM_PATH(state->proxy),
		     STR(buffer));

	/*
	 * Defend against a denial of service attack by limiting the amount
	 * of multi-line text that we are willing to store.
	 */
	if (LEN(state->proxy_buffer) < var_line_limit) {
	    if (VSTRING_LEN(state->proxy_buffer))
		VSTRING_ADDCH(state->proxy_buffer, '\n');
	    vstring_strcat(state->proxy_buffer, STR(buffer));
	}

	/*
	 * Parse the response into code and text. Ignore unrecognized
	 * garbage. This means that any character except space (or end of
	 * line) will have the same effect as the '-' line continuation
	 * character.
	 */
	for (cp = STR(buffer); *cp && ISDIGIT(*cp); cp++)
	     /* void */ ;
	if (cp - STR(buffer) == 3) {
	    if (*cp == '-')
		continue;
	    if (*cp == ' ' || *cp == 0)
		break;
	}
	msg_warn("received garbage from proxy %s: %.100s",
		 VSTREAM_PATH(state->proxy), STR(buffer));
    }

    /*
     * Log a warning in case the proxy does not send the expected response.
     * Silently accept any response when the client expressed no expectation.
     */
    if (expect != SMTPD_PROX_WANT_ANY && expect != *STR(state->proxy_buffer)) {
	va_start(ap, fmt);
	smtpd_proxy_cmd_error(state, fmt, ap);
	va_end(ap);
	return (-1);
    } else {
	return (0);
    }
}

/* smtpd_proxy_rec_put - send message content, rec_put() clone */

int     smtpd_proxy_rec_put(VSTREAM *stream, int rec_type,
			            const char *data, int len)
{
    int     err;

    /*
     * Errors first.
     */
    if (vstream_ftimeout(stream) || vstream_ferror(stream)
	|| vstream_feof(stream))
	return (REC_TYPE_ERROR);
    if ((err = vstream_setjmp(stream)) != 0)
	return (smtpd_proxy_rdwr_error(stream, err), REC_TYPE_ERROR);

    /*
     * Send one content record. Errors and results must be as with rec_put().
     */
    if (rec_type == REC_TYPE_NORM)
	smtp_fputs(data, len, stream);
    else if (rec_type == REC_TYPE_CONT)
	smtp_fwrite(data, len, stream);
    else
	msg_panic("smtpd_proxy_rec_put: need REC_TYPE_NORM or REC_TYPE_CONT");
    return (rec_type);
}

/* smtpd_proxy_rec_fprintf - send message content, rec_fprintf() clone */

int     smtpd_proxy_rec_fprintf(VSTREAM *stream, int rec_type,
				        const char *fmt,...)
{
    va_list ap;
    int     err;

    /*
     * Errors first.
     */
    if (vstream_ftimeout(stream) || vstream_ferror(stream)
	|| vstream_feof(stream))
	return (REC_TYPE_ERROR);
    if ((err = vstream_setjmp(stream)) != 0)
	return (smtpd_proxy_rdwr_error(stream, err), REC_TYPE_ERROR);

    /*
     * Send one content record. Errors and results must be as with
     * rec_fprintf().
     */
    va_start(ap, fmt);
    if (rec_type == REC_TYPE_NORM)
	smtp_vprintf(stream, fmt, ap);
    else
	msg_panic("smtpd_proxy_rec_fprintf: need REC_TYPE_NORM");
    va_end(ap);
    return (rec_type);
}

/* smtpd_proxy_close - close proxy connection */

void    smtpd_proxy_close(SMTPD_STATE *state)
{
    (void) vstream_fclose(state->proxy);
    state->proxy = 0;
}
