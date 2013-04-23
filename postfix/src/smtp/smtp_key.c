/*++
/* NAME
/*	smtp_key 3
/* SUMMARY
/*	cache/table lookup key management
/* SYNOPSIS
/*	#include "smtp.h"
/*
/*	char	*smtp_key_prefix(buffer, delim_na, iterator, context_flags)
/*	VSTRING	*buffer;
/*	const char *delim_na;
/*	SMTP_ITERATOR *iterator;
/*	int	context_flags;
/* DESCRIPTION
/*	The Postfix SMTP server accesses caches and lookup tables,
/*	using lookup keys that contain information from various
/*	contexts: per-server configuration, per-request envelope,
/*	and results from DNS queries.
/*
/*	These lookup keys sometimes share the same context information.
/*	The primary purpose of this API is to ensure that this
/*	shared context is used consistently, and that its use is
/*	made explicit (both are needed to verify that there is no
/*	false cache sharing).
/*
/*	smtp_key_prefix() constructs a lookup key prefix from context
/*	that may be shared with other lookup keys. The user is free
/*	to append additional application-specific context. The result
/*	value is a pointer to the result text.
/*
/*	Arguments:
/* .IP buffer
/*	Storage for the result.
/* .IP delim_na
/*	The field delimiter character, and the optional place holder
/*	character for a) information that is unavailable, b)
/*	information that is inapplicable, or c) that would result
/*	in an empty field.  Key fields that contain "delim_na"
/*	characters will be base64-encoded.
/*	Do not specify "delim_na" characters that are part of the
/*	base64 character set.
/* .IP iterator
/*	Information that will be selected by the specified flags.
/* .IP context_flags
/*	Bit-wise OR of one or more of the following.
/* .RS
/* .IP SMTP_KEY_FLAG_SERVICE
/*	The global service name. This is a proxy for
/*	destination-independent and request-independent context.
/* .IP SMTP_KEY_FLAG_SENDER
/*	The envelope sender address. This is a proxy for sender-dependent
/*	context, such as per-sender SASL authentication. This flag
/*	is ignored when all sender-dependent context is disabled.
/* .IP SMTP_KEY_FLAG_REQ_NEXTHOP
/*	The request nexthop destination. This is a proxy for
/*	destination-dependent, but host-independent context.
/* .IP SMTP_KEY_FLAG_NEXTHOP
/*	The current iterator's nexthop destination (request nexthop
/*	or fallback nexthop, including optional [] and :port). This
/*	is the form that users specify in a SASL or TLS lookup
/*	tables.
/* .IP SMTP_KEY_FLAG_HOSTNAME
/*	The current iterator's remote hostname.
/* .IP SMTP_KEY_FLAG_ADDR
/*	The current iterator's remote address.
/* .IP SMTP_KEY_FLAG_PORT
/*	The current iterator's remote port.
/* .IP SMTP_KEY_FLAG_SASL
/*	The current (obfuscated) SASL login name and password, or
/*	dummy SASL credentials in the case of an object without
/*	SASL authentication.
/*	This option is ignored unless SASL support is compiled in.
/* .IP SMTP_KEY_FLAG_NOSASL
/*	Dummy SASL credentials that match only objects without SASL
/*	authentication.
/*	This option is ignored unless SASL support is compiled in.
/* .RE
/* DIAGNOSTICS
/*	Panic: undefined flag or zero flags. Fatal: out of memory.
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

 /*
  * System library.
  */
#include <sys_defs.h>
#include <netinet/in.h>			/* ntohs() for Solaris or BSD */
#include <arpa/inet.h>			/* ntohs() for Linux or BSD */
#include <string.h>

 /*
  * Utility library.
  */
#include <msg.h>
#include <vstring.h>
#include <base64_code.h>

 /*
  * Global library.
  */
#include <mail_params.h>

 /*
  * Application-specific.
  */
#include <smtp.h>

 /*
  * We use a configurable field terminator and optional place holder for data
  * that is unavailable or inapplicable. We base64-encode content that
  * contains these characters, and content that needs obfuscation.
  */

/* smtp_key_append_na - append place-holder key field */

static void smtp_key_append_na(VSTRING *buffer, const char *delim_na)
{
    if (delim_na[1] != 0)
	VSTRING_ADDCH(buffer, delim_na[1]);
    VSTRING_ADDCH(buffer, delim_na[0]);
}

/* smtp_key_append_base64 - append base64-encoded key field */

static void smtp_key_append_base64(VSTRING *buffer, const char *str,
				           const char *delim_na)
{
    if (str == 0 || str[0] == 0) {
	smtp_key_append_na(buffer, delim_na);
    } else {
	base64_encode_opt(buffer, str, strlen(str), BASE64_FLAG_APPEND);
	VSTRING_ADDCH(buffer, delim_na[0]);
    }
}

/* smtp_key_append_str - append string-valued key field */

static void smtp_key_append_str(VSTRING *buffer, const char *str,
				        const char *delim_na)
{
    if (str == 0 || str[0] == 0) {
	smtp_key_append_na(buffer, delim_na);
    } else if (str[strcspn(str, delim_na)] != 0) {
	base64_encode_opt(buffer, str, strlen(str), BASE64_FLAG_APPEND);
	VSTRING_ADDCH(buffer, delim_na[0]);
    } else {
	vstring_sprintf_append(buffer, "%s%c", str, delim_na[0]);
    }
}

/* smtp_key_append_uint - append unsigned-valued key field */

static void smtp_key_append_uint(VSTRING *buffer, unsigned num,
				         const char *delim_na)
{
    vstring_sprintf_append(buffer, "%u%c", num, delim_na[0]);
}

/* smtp_key_prefix - format common elements in lookup key */

char   *smtp_key_prefix(VSTRING *buffer, const char *delim_na,
			        SMTP_ITERATOR *iter, int flags)
{
    const char myname[] = "smtp_key_prefix";
    SMTP_STATE *state = iter->parent;	/* private member */
    SMTP_SESSION *session;

    /*
     * Sanity checks.
     */
    if (state == 0)
	msg_panic("%s: no parent state", myname);
    if (flags & ~SMTP_KEY_MASK_ALL)
	msg_panic("%s: unknown key flags 0x%x",
		  myname, flags & ~SMTP_KEY_MASK_ALL);
    if (flags == 0)
	msg_panic("%s: zero flags", myname);

    /*
     * Initialize.
     */
    VSTRING_RESET(buffer);

    /*
     * Per-service and per-request context.
     */
    if (flags & SMTP_KEY_FLAG_SERVICE)
	smtp_key_append_str(buffer, state->service, delim_na);
#ifdef USE_SASL_AUTH
    if (flags & SMTP_KEY_FLAG_SENDER) {
	if (var_smtp_sender_auth && *var_smtp_sasl_passwd) {
	    smtp_key_append_str(buffer, state->request->sender, delim_na);
	} else {
	    smtp_key_append_na(buffer, delim_na);	/* sender n/a */
	}
    }
#endif

    /*
     * Per-destination context, non-canonicalized form.
     */
    if (flags & SMTP_KEY_FLAG_REQ_NEXTHOP)
	smtp_key_append_str(buffer, STR(iter->request_nexthop), delim_na);
    if (flags & SMTP_KEY_FLAG_NEXTHOP)
	smtp_key_append_str(buffer, STR(iter->dest), delim_na);

    /*
     * Per-host context, canonicalized form.
     */
    if (flags & SMTP_KEY_FLAG_HOSTNAME)
	smtp_key_append_str(buffer, STR(iter->host), delim_na);
    if (flags & SMTP_KEY_FLAG_ADDR)
	smtp_key_append_str(buffer, STR(iter->addr), delim_na);
    if (flags & SMTP_KEY_FLAG_PORT)
	smtp_key_append_uint(buffer, ntohs(iter->port), delim_na);

    /*
     * Security attributes.
     */
#ifdef USE_SASL_AUTH
    if (flags & SMTP_KEY_FLAG_NOSASL) {
	smtp_key_append_na(buffer, delim_na);	/* username n/a */
	smtp_key_append_na(buffer, delim_na);	/* password n/a */
    }
    if (flags & SMTP_KEY_FLAG_SASL) {
	if ((session = state->session) == 0 || session->sasl_username == 0) {
	    smtp_key_append_na(buffer, delim_na);	/* username n/a */
	    smtp_key_append_na(buffer, delim_na);	/* password n/a */
	} else {
	    smtp_key_append_base64(buffer, session->sasl_username, delim_na);
	    smtp_key_append_base64(buffer, session->sasl_passwd, delim_na);
	}
    }
#endif
    /* Similarly, provide unique TLS fingerprint when applicable. */

    VSTRING_TERMINATE(buffer);

    return STR(buffer);
}