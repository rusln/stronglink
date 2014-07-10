-- Unfortunately, the output of `sqlite3 .dump` is ugly, so this file should be generated by hand.
-- Not to mention, SQLite3 barely supports ALTER TABLE.

CREATE TABLE users (
    user_id INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL,
    username TEXT NOT NULL,
    password_hash TEXT NOT NULL,
    token TEXT,
    user_time INTEGER NOT NULL DEFAULT (strftime('%s'))
);
CREATE UNIQUE INDEX users_unique ON users (username ASC);

CREATE TABLE sessions (
    session_id INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL,
    session_hash TEXT NOT NULL,
    user_id INTEGER NOT NULL,
    session_time INTEGER NOT NULL DEFAULT (strftime('%s'))
);
CREATE INDEX sessions_index ON sessions (user_id ASC);

CREATE TABLE files (
    file_id INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL,
    internal_hash TEXT NOT NULL,
    file_type TEXT NOT NULL,
    file_size INTEGER NOT NULL,
    file_time INTEGER NOT NULL DEFAULT (strftime('%s'))
);
CREATE UNIQUE INDEX files_hash_unique ON files (internal_hash ASC, file_type ASC);

CREATE TABLE uris (
	uri_id INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL,
	uri TEXT NOT NULL
);
CREATE UNIQUE INDEX uris_unique ON uris (uri ASC);

CREATE TABLE links (
	link_id INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL,
	source_uri_id TEXT NOT NULL,
	target_uri_id TEXT NOT NULL,
	meta_file_id INTEGER NOT NULL
);
CREATE UNIQUE INDEX links_unique ON links (source_uri_id ASC, target_uri_id ASC, meta_file_id ASC);
CREATE INDEX links_target_index ON links (target_uri_id ASC);

CREATE TABLE file_uris (
	file_uri_id INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL,
	file_id INTEGER NOT NULL,
	uri_id INTEGER NOT NULL
);
CREATE UNIQUE INDEX file_uris_unique ON file_uris (uri_id ASC, file_id ASC);

-- LOTS of restrictions on column names. Apparently can't use underscores or "fulltext". And the table can't have any non-text columns either, besides the default rowid. So it's hard to give it a meaningful name. `description` isn't quite right because we index titles and potentially other fields too.
CREATE VIRTUAL TABLE fulltext USING "fts4" (
	content="",
	description TEXT
);
CREATE TABLE file_content (
	file_content_id INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL,
	fulltext_rowid INTEGER NOT NULL,
	file_id INTEGER NOT NULL,
	meta_file_id INTEGER NOT NULL
);
CREATE UNIQUE INDEX file_content_unique ON file_content (fulltext_rowid ASC, file_id ASC, meta_file_id);

-- TODO: We need a much better way to handle permissions, especially in order to determine accurate sort orders.
CREATE TABLE file_permissions (
	file_permission_id INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL,
	file_id INTEGER NOT NULL,
	user_id INTEGER NOT NULL,
	meta_file_id INTEGER NOT NULL
);
CREATE UNIQUE INDEX file_permissions_unique ON file_permissions (user_id ASC, file_id ASC, meta_file_id ASC);

CREATE TABLE pulls (
	pull_id INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL,
	user_id INTEGER NOT NULL,
	host TEXT NOT NULL,
	username TEXT NOT NULL,
	password TEXT NOT NULL,
	cookie TEXT NOT NULL,
	query TEXT NOT NULL
);

CREATE TABLE meta_data (
	meta_data_id INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL,
	meta_file_id INTEGER NOT NULL,
	file_id INTEGER NOT NULL,
	field TEXT NOT NULL,
	value TEXT NOT NULL
);
CREATE INDEX meta_data_meta_file_index ON meta_data (meta_file_id ASC);
CREATE INDEX meta_data_file_index ON meta_data (file_id ASC);
CREATE INDEX meta_data_field_index ON meta_data (field ASC);

