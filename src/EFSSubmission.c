#define _GNU_SOURCE
#include <fcntl.h>
#include "async.h"
#include "EarthFS.h"

#define INDEX_MAX (1024 * 100)

struct EFSSubmission {
	EFSRepoRef repo;
	str_t *path;
	str_t *type;
	int64_t size;
	URIListRef URIs;
	str_t *internalHash;
	EFSMetaFileRef meta;
};

EFSSubmissionRef EFSRepoCreateSubmission(EFSRepoRef const repo, strarg_t const type, ssize_t (*read)(void *, byte_t const **), void *const context) {
	if(!repo) return NULL;

	EFSSubmissionRef sub = calloc(1, sizeof(struct EFSSubmission));
	sub->repo = repo;
	sub->type = strdup(type);

	sub->path = EFSRepoCopyTempPath(repo);
	if(async_fs_mkdirp_dirname(sub->path, 0700) < 0) {
		fprintf(stderr, "Error: couldn't create temp dir %s\n", sub->path);
		EFSSubmissionFree(&sub);
		return NULL;
	}

	uv_fs_t req = { .data = co_active() };
	uv_fs_open(loop, &req, sub->path, O_CREAT | O_EXCL | O_TRUNC | O_WRONLY, 0400, async_fs_cb);
	co_switch(yield);
	uv_fs_req_cleanup(&req);
	if(req.result < 0) {
		fprintf(stderr, "Error: couldn't create temp file %s\n", sub->path);
		EFSSubmissionFree(&sub);
		return NULL;
	}
	uv_file const tmp = req.result;

	EFSHasherRef hasher = EFSHasherCreate(sub->type);
	sub->meta = EFSMetaFileCreate(sub->type);

	for(;;) {
		byte_t const *buf = NULL;
		ssize_t const rlen = read(context, &buf);
		if(0 == rlen) break;
		if(rlen < 0) {
			fprintf(stderr, "EFSSubmission read error %d\n", rlen);
			EFSSubmissionFree(&sub);
			goto bail;
		}

		uv_buf_t info = uv_buf_init((char *)buf, rlen);
		uv_fs_write(loop, &req, tmp, &info, 1, sub->size, async_fs_cb);
		co_switch(yield);
		uv_fs_req_cleanup(&req);
		if(req.result < 0) {
			fprintf(stderr, "EFSSubmission write error %d\n", req.result);
			EFSSubmissionFree(&sub);
			goto bail;
		}

		size_t const indexable = MIN(rlen, SUB_ZERO(INDEX_MAX, sub->size));
		EFSHasherWrite(hasher, buf, rlen);
		EFSMetaFileWrite(sub->meta, buf, rlen);

		sub->size += rlen;
	}

	if(!sub->size) {
		fprintf(stderr, "Empty submission\n");
		EFSSubmissionFree(&sub);
		goto bail;
	}

	sub->URIs = EFSHasherEnd(hasher);
	sub->internalHash = strdup(EFSHasherGetInternalHash(hasher));
	EFSMetaFileEnd(sub->meta);

bail:
	EFSHasherFree(&hasher);

	uv_fs_close(loop, &req, tmp, async_fs_cb);
	co_switch(yield);
	uv_fs_req_cleanup(&req);

	return sub;
}
void EFSSubmissionFree(EFSSubmissionRef *const subptr) {
	EFSSubmissionRef sub = *subptr;
	if(!sub) return;
	if(sub->path) {
		uv_fs_t req = { .data = co_active() };
		uv_fs_unlink(loop, &req, sub->path, async_fs_cb);
		co_switch(yield);
		uv_fs_req_cleanup(&req);
	}
	FREE(&sub->path);
	FREE(&sub->type);
	URIListFree(&sub->URIs);
	FREE(&sub->internalHash);
	EFSMetaFileFree(&sub->meta);
	FREE(subptr); sub = NULL;
}
strarg_t EFSSubmissionGetPrimaryURI(EFSSubmissionRef const sub) {
	if(!sub) return NULL;
	return URIListGetURI(sub->URIs, 0);
}

err_t EFSSessionAddSubmission(EFSSessionRef const session, EFSSubmissionRef const sub) {
	if(!session) return 0;
	if(!sub) return -1;

	// TODO: Check session mode
	// TODO: Make sure session repo and submission repo match
	EFSRepoRef const repo = sub->repo;

	str_t *internalPath = EFSRepoCopyInternalPath(repo, sub->internalHash);
	if(async_fs_mkdirp_dirname(internalPath, 0700) < 0) {
		fprintf(stderr, "Couldn't mkdir -p %s\n", internalPath);
		FREE(&internalPath);
		return -1;
	}

	uv_fs_t req = { .data = co_active() };
	uv_fs_link(loop, &req, sub->path, internalPath, async_fs_cb);
	co_switch(yield);
	uv_fs_req_cleanup(&req);
	if(req.result < 0 && -EEXIST != req.result) {
		fprintf(stderr, "Couldn't move %s to %s\n", sub->path, internalPath);
		FREE(&internalPath);
		return -1;
	}
	FREE(&internalPath);

	uv_fs_unlink(loop, &req, sub->path, async_fs_cb);
	co_switch(yield);
	uv_fs_req_cleanup(&req);
	FREE(&sub->path);

	uint64_t const userID = EFSSessionGetUserID(session);
	sqlite3 *const db = EFSRepoDBConnect(repo);

	EXEC(QUERY(db, "BEGIN TRANSACTION"));

	sqlite3_stmt *insertFile = QUERY(db,
		"INSERT OR IGNORE INTO files (internal_hash, file_type, file_size)\n"
		"VALUES (?, ?, ?)");
	sqlite3_bind_text(insertFile, 1, sub->internalHash, -1, SQLITE_STATIC);
	sqlite3_bind_text(insertFile, 2, sub->type, -1, SQLITE_STATIC);
	sqlite3_bind_int64(insertFile, 3, sub->size);
	EXEC(insertFile); insertFile = NULL;

	// We can't use last_insert_rowid() if the file already existed.
	sqlite3_stmt *selectFile = QUERY(db,
		"SELECT file_id FROM files\n"
		"WHERE internal_hash = ? AND file_type = ?");
	sqlite3_bind_text(selectFile, 1, sub->internalHash, -1, SQLITE_STATIC);
	sqlite3_bind_text(selectFile, 2, sub->type, -1, SQLITE_STATIC);
	STEP(selectFile);
	int64_t const fileID = sqlite3_column_int64(selectFile, 0);
	sqlite3_finalize(selectFile); selectFile = NULL;

	sqlite3_stmt *insertURI = QUERY(db,
		"INSERT OR IGNORE INTO uris (uri) VALUES (?)");
	sqlite3_stmt *insertFileURI = QUERY(db,
		"INSERT OR IGNORE INTO file_uris (file_id, uri_id)\n"
		"SELECT ?, uri_id FROM uris WHERE uri = ? LIMIT 1");
	for(index_t i = 0; i < URIListGetCount(sub->URIs); ++i) {
		strarg_t const URI = URIListGetURI(sub->URIs, i);
		sqlite3_bind_text(insertURI, 1, URI, -1, SQLITE_STATIC);
		STEP(insertURI); sqlite3_reset(insertURI);

		sqlite3_bind_int64(insertFileURI, 1, fileID);
		sqlite3_bind_text(insertFileURI, 2, URI, -1, SQLITE_STATIC);
		STEP(insertFileURI); sqlite3_reset(insertFileURI);
	}
	sqlite3_finalize(insertURI); insertURI = NULL;
	sqlite3_finalize(insertFileURI); insertFileURI = NULL;


	// TODO: Add permissions for other specified users too.
	sqlite3_stmt *insertFilePermission = QUERY(db,
		"INSERT OR IGNORE INTO file_permissions\n"
		"	(file_id, user_id, meta_file_id)\n"
		"VALUES (?, ?, ?)");
	sqlite3_bind_int64(insertFilePermission, 1, fileID);
	sqlite3_bind_int64(insertFilePermission, 2, userID);
	sqlite3_bind_int64(insertFilePermission, 3, fileID);
	EXEC(insertFilePermission); insertFilePermission = NULL;

	strarg_t const preferredURI = URIListGetURI(sub->URIs, 0);
	if(EFSMetaFileStore(sub->meta, fileID, preferredURI, db) < 0) {
		fprintf(stderr, "EFSMetaFileStore error\n");
		EXEC(QUERY(db, "ROLLBACK"));
		EFSRepoDBClose(repo, db);
		return -1;
	}

	EXEC(QUERY(db, "COMMIT"));
	EFSRepoDBClose(repo, db);

	return 0;
}

