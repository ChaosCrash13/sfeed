#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "util.h"
#include "xml.h"

#define ISINCONTENT(ctx)  ((ctx).iscontent && !((ctx).iscontenttag))
#define ISCONTENTTAG(ctx) (!((ctx).iscontent) && (ctx).iscontenttag)
/* string and byte-length */
#define STRP(s)           s,sizeof(s)-1

enum FeedType {
	FeedTypeNone = 0,
	FeedTypeRSS  = 1,
	FeedTypeAtom = 2
};
static const char *feedtypes[] = { "", "rss", "atom" };

enum ContentType {
	ContentTypeNone  = 0,
	ContentTypePlain = 1,
	ContentTypeHTML  = 2
};
static const char *contenttypes[] = { "", "plain", "html" };

/* String data / memory pool */
typedef struct string {
	char   *data;   /* data */
	size_t  len;    /* string length */
	size_t  bufsiz; /* allocated size */
} String;

/* NOTE: the order of these fields (content, date, author) indicate the
 *       priority to use them, from least important to high. */
enum TagId {
	TagUnknown = 0,
	/* RSS */
	RSSTagDcdate, RSSTagPubdate,
	RSSTagTitle,
	RSSTagMediaDescription, RSSTagDescription, RSSTagContentEncoded,
	RSSTagGuid,
	RSSTagLink,
	RSSTagAuthor, RSSTagDccreator,
	/* Atom */
	AtomTagUpdated, AtomTagPublished,
	AtomTagTitle,
	AtomTagMediaDescription, AtomTagSummary, AtomTagContent,
	AtomTagId,
	AtomTagLink,
	AtomTagAuthor,
	TagLast,
};

typedef struct feedtag {
	char       *name; /* name of tag to match */
	size_t      len;  /* len of `name` */
	enum TagId  id;   /* unique ID */
} FeedTag;

typedef struct field {
	String     str;
	enum TagId tagid; /* tagid set previously, used for tag priority */
} FeedField;

enum {
	FeedFieldTime = 0, FeedFieldTitle, FeedFieldLink, FeedFieldContent,
	FeedFieldId, FeedFieldAuthor, FeedFieldLast
};

typedef struct feedcontext {
	String          *field;             /* current FeedItem field String */
	FeedField        fields[FeedFieldLast]; /* data for current item */
	enum TagId       tagid;             /* unique number for parsed tag */
	int              iscontent;         /* in content data */
	int              iscontenttag;      /* in content tag */
	enum ContentType contenttype;       /* content-type for item */
	enum FeedType    feedtype;
	int              attrcount; /* count item HTML element attributes */
} FeedContext;

static enum TagId gettag(enum FeedType, const char *, size_t);
static int    gettimetz(const char *, char *, size_t, int *);
static int    isattr(const char *, size_t, const char *, size_t);
static int    istag(const char *, size_t, const char *, size_t);
static int    parsetime(const char *, char *, size_t, time_t *);
static void   printfields(void);
static void   string_append(String *, const char *, size_t);
static void   string_buffer_realloc(String *, size_t);
static void   string_clear(String *);
static void   string_print_encoded(String *);
static void   string_print_trimmed(String *);
static void   xml_handler_attr(XMLParser *, const char *, size_t,
                               const char *, size_t, const char *, size_t);
static void   xml_handler_attr_end(XMLParser *, const char *, size_t,
                                   const char *, size_t);
static void   xml_handler_attr_start(XMLParser *, const char *, size_t,
                                     const char *, size_t);
static void   xml_handler_cdata(XMLParser *, const char *, size_t);
static void   xml_handler_data(XMLParser *, const char *, size_t);
static void   xml_handler_data_entity(XMLParser *, const char *, size_t);
static void   xml_handler_end_el(XMLParser *, const char *, size_t, int);
static void   xml_handler_start_el(XMLParser *, const char *, size_t);
static void   xml_handler_start_el_parsed(XMLParser *, const char *,
                                          size_t, int);

/* map tag type to field */
static int fieldmap[TagLast] = {
	/* RSS */
	[RSSTagDcdate]            = FeedFieldTime,
	[RSSTagPubdate]           = FeedFieldTime,
	[RSSTagTitle]             = FeedFieldTitle,
	[RSSTagMediaDescription]  = FeedFieldContent,
	[RSSTagDescription]       = FeedFieldContent,
	[RSSTagContentEncoded]    = FeedFieldContent,
	[RSSTagGuid]              = FeedFieldId,
	[RSSTagLink]              = FeedFieldLink,
	[RSSTagAuthor]            = FeedFieldAuthor,
	[RSSTagDccreator]         = FeedFieldAuthor,
	/* Atom */
	[AtomTagUpdated]          = FeedFieldTime,
	[AtomTagPublished]        = FeedFieldTime,
	[AtomTagTitle]            = FeedFieldTitle,
	[AtomTagMediaDescription] = FeedFieldContent,
	[AtomTagSummary]          = FeedFieldContent,
	[AtomTagContent]          = FeedFieldContent,
	[AtomTagId]               = FeedFieldId,
	[AtomTagLink]             = FeedFieldLink,
	[AtomTagAuthor]           = FeedFieldAuthor
};

static const int FieldSeparator = '\t'; /* output field seperator character */
static const char *baseurl = "";

static FeedContext ctx;
static XMLParser parser; /* XML parser state */

/* Unique id for parsed tag. */
static enum TagId
gettag(enum FeedType feedtype, const char *name, size_t namelen)
{
	/* RSS, alphabetical order */
	static FeedTag rsstags[] = {
		{ STRP("author"),            RSSTagAuthor            },
		{ STRP("content:encoded"),   RSSTagContentEncoded    },
		{ STRP("dc:creator"),        RSSTagDccreator         },
		{ STRP("dc:date"),           RSSTagDcdate            },
		{ STRP("description"),       RSSTagDescription       },
		{ STRP("guid"),              RSSTagGuid              },
		{ STRP("link"),              RSSTagLink              },
		{ STRP("media:description"), RSSTagMediaDescription  },
		{ STRP("pubdate"),           RSSTagPubdate           },
		{ STRP("title"),             RSSTagTitle             },
		{ NULL, 0, -1 }
	};
	/* Atom, alphabetical order */
	static FeedTag atomtags[] = {
		{ STRP("author"),            AtomTagAuthor           },
		{ STRP("content"),           AtomTagContent          },
		{ STRP("id"),                AtomTagId               },
		{ STRP("link"),              AtomTagLink             },
		{ STRP("media:description"), AtomTagMediaDescription },
		{ STRP("published"),         AtomTagPublished        },
		{ STRP("summary"),           AtomTagSummary          },
		{ STRP("title"),             AtomTagTitle            },
		{ STRP("updated"),           AtomTagUpdated          },
		{ NULL, 0, -1 }
	};
	const FeedTag *tags;
	int i, n;

	/* optimization: these are always non-matching */
	if (namelen < 2 || namelen > 17)
		return TagUnknown;

	switch (feedtype) {
	case FeedTypeRSS:  tags = &rsstags[0];  break;
	case FeedTypeAtom: tags = &atomtags[0]; break;
	default:           return TagUnknown;
	}
	/* search */
	for (i = 0; tags[i].name; i++) {
		if (!(n = strcasecmp(tags[i].name, name)))
			return tags[i].id; /* found */
		/* optimization: tags are sorted so nothing after matches. */
		if (n > 0)
			return TagUnknown;
	}
	return TagUnknown; /* NOTREACHED */
}

/* Clear string only; don't free, prevents unnecessary reallocation. */
static void
string_clear(String *s)
{
	if (s->data)
		s->data[0] = '\0';
	s->len = 0;
}

static void
string_buffer_realloc(String *s, size_t newlen)
{
	char *p;
	size_t alloclen;

	for (alloclen = 64; alloclen <= newlen; alloclen *= 2)
		;
	if (!(p = realloc(s->data, alloclen)))
		err(1, "realloc");
	s->bufsiz = alloclen;
	s->data = p;
}

static void
string_append(String *s, const char *data, size_t len)
{
	if (!len || *data == '\0')
		return;
	/* check if allocation is necesary, don't shrink buffer,
	 * should be more than bufsiz ofcourse. */
	if (s->len + len >= s->bufsiz)
		string_buffer_realloc(s, s->len + len + 1);
	memcpy(s->data + s->len, data, len);
	s->len += len;
	s->data[s->len] = '\0';
}

/* Get timezone from string, return as formatted string and time offset,
 * for the offset it assumes UTC.
 * NOTE: only parses timezones in RFC-822, other timezones are ambiguous
 * anyway. If needed you can add some yourself, like "cest", "cet" etc. */
static int
gettimetz(const char *s, char *buf, size_t bufsiz, int *tzoffset)
{
	static struct tzone {
		char *name;
		int offhour;
		int offmin;
	} tzones[] = {
		{ "CDT", -5, 0 },
		{ "CST", -6, 0 },
		{ "EDT", -4, 0 },
		{ "EST", -5, 0 },
		{ "GMT",  0, 0 },
		{ "MDT", -6, 0 },
		{ "MST", -7, 0 },
		{ "PDT", -7, 0 },
		{ "PST", -8, 0 },
		{ "UT",   0, 0 },
		{ "UTC",  0, 0 },
		{ "A",   -1, 0 },
		{ "M",  -12, 0 },
		{ "N",    1, 0 },
		{ "Y",   12, 0 },
		{ "Z",    0, 0 }
	};
	char tzbuf[5] = "", *tz = "", c = '+';
	int tzhour = 0, tzmin = 0, r;
	size_t i;

	/* skip milliseconds for: %Y-%m-%dT%H:%M:%S.000Z */
	if (*s == '.') {
		for (s++; *s && isdigit((int)*s); s++)
			;
	}
	if (!*s || *s == 'Z' || *s == 'z')
		goto time_ok;
	/* skip whitespace */
	s = &s[strspn(s, " \t")];

	/* look until some common timezone delimiters are found */
	for (i = 0; s[i] && isalpha((int)s[i]); i++)
		;
	/* copy tz name */
	if (i >= sizeof(tzbuf))
		return -1; /* timezone too long */
	memcpy(tzbuf, s, i);
	tzbuf[i] = '\0';

	if ((sscanf(s, "%c%02d:%02d", &c, &tzhour, &tzmin)) == 3)
		;
	else if (sscanf(s, "%c%02d%02d", &c, &tzhour, &tzmin) == 3)
		;
	else if (sscanf(s, "%c%d", &c, &tzhour) == 2)
		tzmin = 0;
	else
		tzhour = tzmin = 0;
	if (!tzhour && !tzmin)
		c = '+';

	/* compare tz and adjust offset relative to UTC */
	for (i = 0; i < LEN(tzones); i++) {
		if (!strcmp(tzbuf, tzones[i].name)) {
			tz = "UTC";
			tzhour = tzones[i].offhour;
			tzmin = tzones[i].offmin;
			c = tzones[i].offhour < 0 ? '-' : '+';
			break;
		}
	}
	tzhour = abs(tzhour);
	tzmin = abs(tzmin);

time_ok:
	/* timezone set but non-match */
	if (tzbuf[0] && !tz[0]) {
		if (strlcpy(buf, tzbuf, bufsiz) >= bufsiz)
			return -1; /* truncation */
		tzhour = tzmin = 0;
		c = '+';
	} else {
		r = snprintf(buf, bufsiz, "%s%c%02d:%02d",
		             tz[0] ? tz : "UTC", c, tzhour, tzmin);
		if (r < 0 || (size_t)r >= bufsiz)
			return -1; /* truncation or error */
	}
	if (tzoffset)
		*tzoffset = ((tzhour * 3600) + (tzmin * 60)) *
		            (c == '-' ? -1 : 1);
	return 0;
}

static int
parsetime(const char *s, char *buf, size_t bufsiz, time_t *tp)
{
	time_t t;
	struct tm tm;
	const char *formats[] = {
		"%a, %d %b %Y %H:%M:%S",
		"%Y-%m-%d %H:%M:%S",
		"%Y-%m-%dT%H:%M:%S",
		NULL
	};
	char tz[16], *p;
	size_t i;
	int tzoffset, r;

	for (i = 0; formats[i]; i++) {
		if (!(p = strptime(s, formats[i], &tm)))
			continue;
		tm.tm_isdst = -1; /* don't use DST */
		if ((t = mktime(&tm)) == -1) /* error */
			return -1;
		if (gettimetz(p, tz, sizeof(tz), &tzoffset) == -1)
			return -1;
		t -= tzoffset;
		if (buf) {
			r = snprintf(buf, bufsiz,
			         "%04d-%02d-%02d %02d:%02d:%02d %s",
			         tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
			         tm.tm_hour, tm.tm_min, tm.tm_sec, tz);
			if (r == -1 || (size_t)r >= bufsiz)
				return -1; /* truncation */
		}
		if (tp)
			*tp = t;
		return 0;
	}
	return -1;
}

/* Print text, encode TABs, newlines and '\', remove other whitespace.
 * Remove leading and trailing whitespace. */
static void
string_print_encoded(String *s)
{
	const char *p, *e;

	if (!s->data || !s->len)
		return;

	/* skip leading whitespace */
	for (p = s->data; *p && isspace((int)*p); p++)
		;
	/* seek offset of trailing whitespace */
	for (e = p + strlen(p); e > p && isspace((int)*(e - 1)); e--)
		;

	for (; *p && p != e; p++) {
		switch (*p) {
		case '\n': fputs("\\n",  stdout); break;
		case '\\': fputs("\\\\", stdout); break;
		case '\t': fputs("\\t",  stdout); break;
		default:
			/* ignore control chars */
			if (!iscntrl((int)*p))
				putchar(*p);
			break;
		}
	}
}

/* Print text, replace TABs, carriage return and other whitespace with ' '.
 * Other control chars are removed. Remove leading and trailing whitespace. */
static void
string_print_trimmed(String *s)
{
	const char *p, *e;

	if (!s->data || !s->len)
		return;

	/* skip leading whitespace */
	for (p = s->data; *p && isspace((int)*p); p++)
		;
	/* seek offset of trailing whitespace */
	for (e = p + strlen(p); e > p && isspace((int)*(e - 1)); e--)
		;

	for (; *p && p != e; p++) {
		if (isspace((int)*p))
			putchar(' '); /* any whitespace to space */
		else if (!iscntrl((int)*p))
			/* ignore other control chars */
			putchar((int)*p);
	}
}

static void
printfields(void)
{
	char link[4096], timebuf[64];
	time_t t;
	int r = -1;

	/* parse time, timestamp and formatted timestamp field is empty
	 * if the parsed time is invalid */
	if (ctx.fields[FeedFieldTime].str.data)
		r = parsetime(ctx.fields[FeedFieldTime].str.data,
		              timebuf, sizeof(timebuf), &t);
	if (r != -1)
		printf("%ld", (long)t);
	putchar(FieldSeparator);
	if (r != -1)
		fputs(timebuf, stdout);
	putchar(FieldSeparator);
	string_print_trimmed(&ctx.fields[FeedFieldTitle].str);
	putchar(FieldSeparator);
	/* always print absolute urls */
	if (ctx.fields[FeedFieldLink].str.data &&
	    absuri(ctx.fields[FeedFieldLink].str.data, baseurl, link,
	           sizeof(link)) != -1)
		fputs(link, stdout);
	putchar(FieldSeparator);
	string_print_encoded(&ctx.fields[FeedFieldContent].str);
	putchar(FieldSeparator);
	fputs(contenttypes[ctx.contenttype], stdout);
	putchar(FieldSeparator);
	string_print_trimmed(&ctx.fields[FeedFieldId].str);
	putchar(FieldSeparator);
	string_print_trimmed(&ctx.fields[FeedFieldAuthor].str);
	putchar(FieldSeparator);
	fputs(feedtypes[ctx.feedtype], stdout);
	putchar('\n');
}

static int
istag(const char *name, size_t len, const char *name2, size_t len2)
{
	return (len == len2 && !strcasecmp(name, name2));
}

static int
isattr(const char *name, size_t len, const char *name2, size_t len2)
{
	return (len == len2 && !strcasecmp(name, name2));
}

static void
xml_handler_attr(XMLParser *p, const char *tag, size_t taglen,
	const char *name, size_t namelen, const char *value,
	size_t valuelen)
{
	(void)tag;
	(void)taglen;

	/* handles transforming inline XML to data */
	if (ISINCONTENT(ctx)) {
		if (ctx.contenttype == ContentTypeHTML)
			xml_handler_data(p, value, valuelen);
		return;
	}

	if (ctx.feedtype == FeedTypeAtom) {
		if (ISCONTENTTAG(ctx)) {
			if (isattr(name, namelen, STRP("type")) &&
			   (isattr(value, valuelen, STRP("xhtml")) ||
			    isattr(value, valuelen, STRP("text/xhtml")) ||
			    isattr(value, valuelen, STRP("html")) ||
			    isattr(value, valuelen, STRP("text/html"))))
			{
				ctx.contenttype = ContentTypeHTML;
			}
		} else if (ctx.tagid == AtomTagLink &&
		          isattr(name, namelen, STRP("href")))
		{
			/* link href attribute */
			string_append(&ctx.fields[FeedFieldLink].str, value, valuelen);
		}
	}
}

static void
xml_handler_attr_end(XMLParser *p, const char *tag, size_t taglen,
	const char *name, size_t namelen)
{
	(void)tag;
	(void)taglen;
	(void)name;
	(void)namelen;

	if (!ISINCONTENT(ctx) || ctx.contenttype != ContentTypeHTML)
		return;

	/* handles transforming inline XML to data */
	xml_handler_data(p, "\"", 1);
	ctx.attrcount = 0;
}

static void
xml_handler_attr_start(XMLParser *p, const char *tag, size_t taglen,
	const char *name, size_t namelen)
{
	(void)tag;
	(void)taglen;

	if (!ISINCONTENT(ctx) || ctx.contenttype != ContentTypeHTML)
		return;

	/* handles transforming inline XML to data */
	if (!ctx.attrcount)
		xml_handler_data(p, " ", 1);
	ctx.attrcount++;
	xml_handler_data(p, name, namelen);
	xml_handler_data(p, "=\"", 2);
}

static void
xml_handler_cdata(XMLParser *p, const char *s, size_t len)
{
	(void)p;

	if (!ctx.field)
		return;

	string_append(ctx.field, s, len);
}

/* NOTE: this handler can be called multiple times if the data in this
 *       block is bigger than the buffer. */
static void
xml_handler_data(XMLParser *p, const char *s, size_t len)
{
	if (!ctx.field)
		return;

	/* add only data from <name> inside <author> tag
	 * or any other non-<author> tag */
	if (ctx.tagid != AtomTagAuthor || !strcmp(p->tag, "name"))
		string_append(ctx.field, s, len);
}

static void
xml_handler_data_entity(XMLParser *p, const char *data, size_t datalen)
{
	char buffer[16];
	ssize_t len;

	if (!ctx.field)
		return;

	/* try to translate entity, else just pass as data to
	 * xml_data_handler. */
	len = xml_entitytostr(data, buffer, sizeof(buffer));
	/* this should never happen (buffer too small) */
	if (len < 0)
		return;

	if (len > 0)
		xml_handler_data(p, buffer, (size_t)len);
	else
		xml_handler_data(p, data, datalen);
}

static void
xml_handler_start_el(XMLParser *p, const char *name, size_t namelen)
{
	enum TagId tagid;

	if (ISINCONTENT(ctx)) {
		ctx.attrcount = 0;
		if (ctx.contenttype == ContentTypeHTML) {
			xml_handler_data(p, "<", 1);
			xml_handler_data(p, name, namelen);
		}
		return;
	}

	/* start of RSS or Atom item / entry */
	if (ctx.feedtype == FeedTypeNone) {
		if (istag(name, namelen, STRP("entry"))) {
			/* Atom */
			ctx.feedtype = FeedTypeAtom;
			/* default content type for Atom */
			ctx.contenttype = ContentTypePlain;
		} else if (istag(name, namelen, STRP("item"))) {
			/* RSS */
			ctx.feedtype = FeedTypeRSS;
			/* default content type for RSS */
			ctx.contenttype = ContentTypeHTML;
		}
		return;
	}

	/* field tagid already set: return */
	if (ctx.tagid)
		return;

	/* in item */
	tagid = gettag(ctx.feedtype, name, namelen);
	ctx.tagid = tagid;

	/* map tag type to field: unknown or less priority is ignored. */
	if (tagid <= ctx.fields[fieldmap[ctx.tagid]].tagid) {
		ctx.field = NULL;
		return;
	}
	ctx.iscontenttag = (fieldmap[ctx.tagid] == FeedFieldContent);
	ctx.field = &(ctx.fields[fieldmap[ctx.tagid]].str);
	ctx.fields[fieldmap[ctx.tagid]].tagid = tagid;
	/* clear field */
	string_clear(ctx.field);
}

static void
xml_handler_start_el_parsed(XMLParser *p, const char *tag, size_t taglen,
	int isshort)
{
	(void)tag;
	(void)taglen;

	if (ctx.iscontenttag) {
		ctx.iscontent = 1;
		ctx.iscontenttag = 0;
		return;
	}

	if (!ISINCONTENT(ctx) || ctx.contenttype != ContentTypeHTML)
		return;

	if (isshort)
		xml_handler_data(p, "/>", 2);
	else
		xml_handler_data(p, ">", 1);
}

static void
xml_handler_end_el(XMLParser *p, const char *name, size_t namelen, int isshort)
{
	size_t i;

	if (ctx.feedtype == FeedTypeNone)
		return;

	if (ISINCONTENT(ctx)) {
		/* not close content field */
		if (gettag(ctx.feedtype, name, namelen) != ctx.tagid) {
			if (!isshort && ctx.contenttype == ContentTypeHTML) {
				xml_handler_data(p, "</", 2);
				xml_handler_data(p, name, namelen);
				xml_handler_data(p, ">", 1);
			}
			return;
		}
	} else if (!ctx.tagid && ((ctx.feedtype == FeedTypeAtom &&
	   istag(name, namelen, STRP("entry"))) || /* Atom */
	   (ctx.feedtype == FeedTypeRSS &&
	   istag(name, namelen, STRP("item"))))) /* RSS */
	{
		/* end of RSS or Atom entry / item */
		printfields();

		/* clear strings */
		for (i = 0; i < FeedFieldLast; i++) {
			string_clear(&ctx.fields[i].str);
			ctx.fields[i].tagid = TagUnknown;
		}
		ctx.contenttype = ContentTypeNone;
		/* allow parsing of Atom and RSS in one XML stream. */
		ctx.feedtype = FeedTypeNone;
	} else if (!ctx.tagid ||
	           gettag(ctx.feedtype, name, namelen) != ctx.tagid) {
		/* not end of field */
		return;
	}
	/* close field */
	ctx.iscontent = 0;
	ctx.tagid = TagUnknown;
	ctx.field = NULL;
}

int
main(int argc, char *argv[])
{
	if (argc > 1)
		baseurl = argv[1];

	if (setenv("TZ", "UTC", 1) == -1)
		err(1, "setenv");
	tzset();

	parser.xmlattr = xml_handler_attr;
	parser.xmlattrend = xml_handler_attr_end;
	parser.xmlattrstart = xml_handler_attr_start;
	parser.xmlcdata = xml_handler_cdata;
	parser.xmldata = xml_handler_data;
	parser.xmldataentity = xml_handler_data_entity;
	parser.xmltagend = xml_handler_end_el;
	parser.xmltagstart = xml_handler_start_el;
	parser.xmltagstartparsed = xml_handler_start_el_parsed;

	parser.getnext = getchar;
	xml_parse(&parser);

	return 0;
}
