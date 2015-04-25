#include <assert.h>
#include "common.h"
#include "StrongLink.h"
#include "http/HTTPServer.h"
#include "http/HTTPHeaders.h"
#include "http/MultipartForm.h"
#include "http/QueryString.h"

#define QUERY_BATCH_SIZE 50
#define AUTH_FORM_MAX 1023

// TODO: Put this somewhere.
bool URIPath(strarg_t const URI, strarg_t const path, strarg_t *const qs) {
	size_t len = prefix(path, URI);
	if(!len) return false;
	if('/' == URI[len]) len++;
	if('\0' != URI[len] && '?' != URI[len]) return false;
	if(qs) *qs = URI + len;
	return true;
}



static int POST_auth(SLNRepoRef const repo, SLNSessionRef const session, HTTPConnectionRef const conn, HTTPMethod const method, strarg_t const URI, HTTPHeadersRef const headers) {
	if(HTTP_POST != method) return -1;
	if(!URIPath(URI, "/sln/auth", NULL)) return -1;

	str_t formdata[AUTH_FORM_MAX+1];
	ssize_t len = HTTPConnectionReadBodyStatic(conn, (byte_t *)formdata, AUTH_FORM_MAX);
	if(UV_EMSGSIZE == len) return 413; // Request Entity Too Large
	if(len < 0) return 500;
	formdata[len] = '\0';

	SLNSessionCacheRef const cache = SLNRepoGetSessionCache(repo);
	static strarg_t const fields[] = {
		"user",
		"pass",
		"token", // TODO: CSRF protection
	};
	str_t *values[numberof(fields)] = {};
	QSValuesParse(formdata, values, fields, numberof(fields));
	SLNSessionRef s;
	int rc = SLNSessionCacheCreateSession(cache, values[0], values[1], &s);
	QSValuesCleanup(values, numberof(values));

	if(DB_SUCCESS != rc) return 403;

	str_t *cookie = SLNSessionCopyCookie(s);
	SLNSessionRelease(&s);
	if(!cookie) return 500;

	HTTPConnectionWriteResponse(conn, 200, "OK");
	HTTPConnectionWriteSetCookie(conn, cookie, "/", 60 * 60 * 24 * 365);
	HTTPConnectionWriteContentLength(conn, 0);
	HTTPConnectionBeginBody(conn);
	HTTPConnectionEnd(conn);

	FREE(&cookie);
	return 0;
}
static int GET_file(SLNRepoRef const repo, SLNSessionRef const session, HTTPConnectionRef const conn, HTTPMethod const method, strarg_t const URI, HTTPHeadersRef const headers) {
	if(HTTP_GET != method && HTTP_HEAD != method) return -1;

	int len = 0;
	str_t algo[SLN_ALGO_SIZE];
	str_t hash[SLN_HASH_SIZE];
	algo[0] = '\0';
	hash[0] = '\0';
	sscanf(URI, "/sln/file/" SLN_ALGO_FMT "/" SLN_HASH_FMT "%n", algo, hash, &len);
	if(!algo[0] || !hash[0]) return -1;
	if('/' == URI[len]) len++;
	if('\0' != URI[len] && '?' != URI[len]) return -1;

	// TODO: Check for conditional get headers and return 304 Not Modified.

	str_t fileURI[SLN_URI_MAX];
	int rc = snprintf(fileURI, SLN_URI_MAX, "hash://%s/%s", algo, hash);
	if(rc < 0 || rc >= SLN_URI_MAX) return 500;

	SLNFileInfo info[1];
	rc = SLNSessionGetFileInfo(session, fileURI, info);
	if(DB_EACCES == rc) return 403;
	if(DB_NOTFOUND == rc) return 404;
	if(DB_SUCCESS != rc) return 500;

	uv_file file = async_fs_open(info->path, O_RDONLY, 0000);
	if(UV_ENOENT == file) {
		SLNFileInfoCleanup(info);
		return 410; // Gone
	}
	if(file < 0) {
		SLNFileInfoCleanup(info);
		return 500;
	}

	// TODO: Hosting untrusted data is really hard.
	// The same origin policy isn't doing us any favors, because most
	// simple installations won't have a second domain to use.
	// - For loopback, we can use an alternate loopback IP, but that
	//   still only covers a limited range of use cases.
	// - We would really like suborigins, if they're ever widely supported.
	//   <http://www.chromium.org/developers/design-documents/per-page-suborigins>
	// - We could report untrusted types as "text/plain" or
	//   "application/binary", which would be inconvenient but
	//   very reliable.

	// TODO: Use Content-Disposition to suggest a filename, for file types
	// that aren't useful to view inline.

	HTTPConnectionWriteResponse(conn, 200, "OK");
	HTTPConnectionWriteContentLength(conn, info->size);
	HTTPConnectionWriteHeader(conn, "Content-Type", info->type);
	HTTPConnectionWriteHeader(conn, "Cache-Control", "max-age=31536000");
	HTTPConnectionWriteHeader(conn, "ETag", "1");
//	HTTPConnectionWriteHeader(conn, "Accept-Ranges", "bytes"); // TODO
	HTTPConnectionWriteHeader(conn, "Content-Security-Policy", "'none'");
	HTTPConnectionWriteHeader(conn, "X-Content-Type-Options", "nosniff");
	HTTPConnectionBeginBody(conn);
	if(HTTP_HEAD != method) {
		HTTPConnectionWriteFile(conn, file);
	}
	HTTPConnectionEnd(conn);

	SLNFileInfoCleanup(info);
	async_fs_close(file);
	return 0;
}
static int POST_file(SLNRepoRef const repo, SLNSessionRef const session, HTTPConnectionRef const conn, HTTPMethod const method, strarg_t const URI, HTTPHeadersRef const headers) {
	if(HTTP_POST != method) return -1;

	int len = 0;
	sscanf(URI, "/sln/file%n", &len);
	if(!len) return -1;
	if('/' == URI[len]) len++;
	if('\0' != URI[len] && '?' != URI[len]) return -1;

	strarg_t const type = HTTPHeadersGet(headers, "Content-Type");
	if(!type) return 400;

	SLNSubmissionRef sub;
	int rc = SLNSubmissionCreate(session, type, &sub);
	if(UV_EACCES == rc) return 403;
	if(rc < 0) return 500;
	for(;;) {
		uv_buf_t buf[1] = {};
		rc = HTTPConnectionReadBody(conn, buf);
		if(rc < 0) {
			SLNSubmissionFree(&sub);
			return 0;
		}
		if(0 == buf->len) break;
		rc = SLNSubmissionWrite(sub, (byte_t *)buf->base, buf->len);
		if(rc < 0) {
			SLNSubmissionFree(&sub);
			return 500;
		}
	}
	rc = SLNSubmissionEnd(sub);
	if(rc < 0) {
		SLNSubmissionFree(&sub);
		return 500;
	}
	rc = SLNSubmissionBatchStore(&sub, 1);
	if(rc < 0) {
		SLNSubmissionFree(&sub);
		return 500;
	}
	strarg_t const location = SLNSubmissionGetPrimaryURI(sub);
	if(!location) {
		SLNSubmissionFree(&sub);
		return 500;
	}

	HTTPConnectionWriteResponse(conn, 201, "Created");
	HTTPConnectionWriteHeader(conn, "X-Location", location);
	// TODO: X-Content-Address or something? Or X-Name?
	HTTPConnectionWriteContentLength(conn, 0);
	HTTPConnectionBeginBody(conn);
	HTTPConnectionEnd(conn);
	SLNSubmissionFree(&sub);
	if(rc < 0) return 500;
	return 0;
}

static int sendURIBatch(SLNSessionRef const session, SLNFilterRef const filter, SLNFilterOpts *const opts, HTTPConnectionRef const conn) {
	size_t count;
	str_t *URIs[QUERY_BATCH_SIZE];
	opts->count = QUERY_BATCH_SIZE;
	int rc = SLNSessionCopyFilteredURIs(session, filter, opts, URIs, &count);
	if(DB_SUCCESS != rc) return rc;
	if(0 == count) return DB_NOTFOUND;
	rc = 0;
	for(size_t i = 0; i < count; i++) {
		if(rc >= 0) {
			uv_buf_t const parts[] = {
				uv_buf_init((char *)URIs[i], strlen(URIs[i])),
				uv_buf_init("\r\n", 2),
			};
			rc = HTTPConnectionWriteChunkv(conn, parts, numberof(parts));
		}
		FREE(&URIs[i]);
	}
	if(rc < 0) return DB_EIO;
	return DB_SUCCESS;
}
static void sendURIList(SLNSessionRef const session, SLNFilterRef const filter, strarg_t const qs, HTTPConnectionRef const conn) {
	SLNFilterOpts opts[1];
	SLNFilterOptsParse(qs, +1, 0, opts);

	// We're sending a series of batches, so reversing one batch
	// doesn't make sense.
	opts->outdir = opts->dir;

	// I'm aware that we're abusing HTTP for sending real-time push data.
	// I'd also like to support WebSocket at some point, but this is simpler
	// and frankly probably more widely supported.
	// Note that the protocol doesn't really break even if this data is
	// cached. It DOES break if a proxy tries to buffer the whole response
	// before passing it back to the client. I'd be curious to know whether
	// such proxies still exist in 2015.
	HTTPConnectionWriteResponse(conn, 200, "OK");
	HTTPConnectionWriteHeader(conn, "Transfer-Encoding", "chunked");
	HTTPConnectionWriteHeader(conn,
		"Content-Type", "text/uri-list; charset=utf-8");
	HTTPConnectionWriteHeader(conn, "Cache-Control", "no-store");
	HTTPConnectionWriteHeader(conn, "Vary", "*");
	HTTPConnectionBeginBody(conn);
	int rc;

	for(;;) {
		rc = sendURIBatch(session, filter, opts, conn);
		if(DB_NOTFOUND == rc) break;
		if(DB_SUCCESS == rc) continue;
		goto cleanup;
	}

	// If we're going backwards, we obviously won't reach the end where
	// new items are appearing.
	// TODO: There should be a flag to not enter real-time mode even if
	// we're going forwards.
	if(opts->dir < 0) goto cleanup;

	SLNRepoRef const repo = SLNSessionGetRepo(session);
	for(;;) {
		uint64_t const timeout = uv_now(loop)+(1000 * 30);
		rc = SLNRepoSubmissionWait(repo, opts->sortID, timeout);
		if(UV_ETIMEDOUT == rc) {
			uv_buf_t const parts[] = { uv_buf_init("\r\n", 2) };
			rc = HTTPConnectionWriteChunkv(conn, parts, numberof(parts));
			if(rc < 0) break;
			continue;
		}
		assert(rc >= 0); // TODO: Handle cancellation?

		for(;;) {
			rc = sendURIBatch(session, filter, opts, conn);
			if(DB_NOTFOUND == rc) break;
			if(DB_SUCCESS == rc) continue;
			fprintf(stderr, "Query error: %s\n", db_strerror(rc));
			goto cleanup;
		}
	}

cleanup:
	HTTPConnectionWriteChunkv(conn, NULL, 0);
	HTTPConnectionEnd(conn);
	SLNFilterOptsCleanup(opts);
}
static int parseFilter(SLNSessionRef const session, HTTPConnectionRef const conn, HTTPMethod const method, HTTPHeadersRef const headers, SLNFilterRef *const out) {
	if(HTTP_POST != method) return SLNFilterCreate(session, SLNAllFilterType, out);
	// TODO: Check Content-Type header for JSON.
	SLNJSONFilterParserRef parser;
	int rc = SLNJSONFilterParserCreate(session, &parser);
	if(DB_SUCCESS != rc) return rc;
	for(;;) {
		uv_buf_t buf[1];
		rc = HTTPConnectionReadBody(conn, buf);
		if(rc < 0) {
			SLNJSONFilterParserFree(&parser);
			return DB_ENOMEM;
		}
		if(0 == buf->len) break;

		SLNJSONFilterParserWrite(parser, (str_t const *)buf->base, buf->len);
	}
	*out = SLNJSONFilterParserEnd(parser);
	SLNJSONFilterParserFree(&parser);
	if(!*out) return DB_ENOMEM;
	return DB_SUCCESS;
}
static int GET_query(SLNRepoRef const repo, SLNSessionRef const session, HTTPConnectionRef const conn, HTTPMethod const method, strarg_t const URI, HTTPHeadersRef const headers) {
	if(HTTP_GET != method && HTTP_HEAD != method) return -1;
	strarg_t qs;
	if(!URIPath(URI, "/sln/query", &qs)) return -1;

	SLNFilterRef filter = NULL;
	int rc;

	static strarg_t const fields[] = {
		"q",
	};
	str_t *values[numberof(fields)] = {};
	QSValuesParse(qs, values, fields, numberof(fields));
	rc = SLNUserFilterParse(session, values[0], &filter);
	QSValuesCleanup(values, numberof(values));
	if(DB_EINVAL == rc) rc = SLNFilterCreate(session, SLNAllFilterType, &filter);
	if(DB_EACCES == rc) return 403;
	if(DB_SUCCESS != rc) return 500;

	sendURIList(session, filter, qs, conn);
	SLNFilterFree(&filter);
	return 0;
}
static int POST_query(SLNRepoRef const repo, SLNSessionRef const session, HTTPConnectionRef const conn, HTTPMethod const method, strarg_t const URI, HTTPHeadersRef const headers) {
	if(HTTP_POST != method) return -1;
	strarg_t qs;
	if(!URIPath(URI, "/sln/query", &qs)) return -1;

	SLNFilterRef filter;
	int rc = parseFilter(session, conn, method, headers, &filter);
	if(DB_EACCES == rc) return 403;
	if(DB_SUCCESS != rc) return 500;
	sendURIList(session, filter, qs, conn);
	SLNFilterFree(&filter);
	return 0;
}
static int GET_metafiles(SLNRepoRef const repo, SLNSessionRef const session, HTTPConnectionRef const conn, HTTPMethod const method, strarg_t const URI, HTTPHeadersRef const headers) {
	if(HTTP_GET != method) return -1;
	strarg_t qs;
	if(!URIPath(URI, "/sln/metafiles", &qs)) return -1;

	SLNFilterRef filter;
	int rc = SLNFilterCreate(session, SLNMetaFileFilterType, &filter);
	if(DB_EACCES == rc) return 403;
	if(DB_SUCCESS != rc) return 500;
	sendURIList(session, filter, qs, conn); // TODO: Use "meta -> file" format
	SLNFilterFree(&filter);
	return 0;
}
// TODO: Remove this once we rewrite the sync system not to need it.
static int GET_query_obsolete(SLNRepoRef const repo, SLNSessionRef const session, HTTPConnectionRef const conn, HTTPMethod const method, strarg_t const URI, HTTPHeadersRef const headers) {
	if(HTTP_GET != method) return -1;
	strarg_t qs;
	if(!URIPath(URI, "/sln/query-obsolete", &qs)) return -1;

	SLNFilterRef filter, subfilter;
	int rc = SLNFilterCreate(session, SLNBadMetaFileFilterType, &filter);
	if(DB_EACCES == rc) return 403;
	if(DB_SUCCESS != rc) return 500;

	static strarg_t const fields[] = {
		"q",
	};
	str_t *values[numberof(fields)] = {};
	QSValuesParse(qs, values, fields, numberof(fields));
	rc = SLNUserFilterParse(session, values[0], &subfilter);
	QSValuesCleanup(values, numberof(values));
	if(DB_EINVAL == rc) rc = SLNFilterCreate(session, SLNAllFilterType, &subfilter);
	if(DB_SUCCESS != rc) {
		SLNFilterFree(&filter);
		return 500;
	}

	rc = SLNFilterAddFilterArg(filter, subfilter);
	if(rc < 0) {
		SLNFilterFree(&subfilter);
		SLNFilterFree(&filter);
		return 500;
	}

	sendURIList(session, filter, qs, conn);
	SLNFilterFree(&filter);
	return 0;
}


int SLNServerDispatch(SLNRepoRef const repo, SLNSessionRef const session, HTTPConnectionRef const conn, HTTPMethod const method, strarg_t const URI, HTTPHeadersRef const headers) {
	int rc = -1;
	rc = rc >= 0 ? rc : POST_auth(repo, session, conn, method, URI, headers);
	rc = rc >= 0 ? rc : GET_file(repo, session, conn, method, URI, headers);
	rc = rc >= 0 ? rc : POST_file(repo, session, conn, method, URI, headers);
	rc = rc >= 0 ? rc : GET_query(repo, session, conn, method, URI, headers);
	rc = rc >= 0 ? rc : POST_query(repo, session, conn, method, URI, headers);
	rc = rc >= 0 ? rc : GET_metafiles(repo, session, conn, method, URI, headers);
	rc = rc >= 0 ? rc : GET_query_obsolete(repo, session, conn, method, URI, headers);
	return rc;
}

