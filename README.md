# mutt-msqlite

Work-in-progress support for using `mutt` against a local SQLite database
containing a mirror of a Gmail account.  Updating the database itself is out of
scope -- I currently have some awful Node programs which use the Gmail API to
incrementally update the database.

## SQLite Schema

CREATE TABLE label (
	id TEXT PRIMARY KEY,
	name TEXT NOT NULL,
	type TEXT NOT NULL);

CREATE TABLE message (
	id TEXT PRIMARY KEY,
	thread_id TEXT NOT NULL,
	history_id INTEGER,
	internal_date INTEGER,
	death_time INTEGER);

CREATE TABLE message_raw (
	id TEXT PRIMARY KEY REFERENCES message (id) ON DELETE CASCADE,
	raw TEXT NOT NULL);

CREATE TABLE message_to_label (
	message_id TEXT REFERENCES message (id) ON DELETE CASCADE,
	label_id TEXT REFERENCES label (id) ON DELETE CASCADE,
	PRIMARY KEY (message_id, label_id));

## The Real Mutt

For the real Mutt, see the Mutt home page:

 http://www.mutt.org/
