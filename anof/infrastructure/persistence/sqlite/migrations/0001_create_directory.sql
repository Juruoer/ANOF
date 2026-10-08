-- 创建目录需要的最小元数据；已应用后通过新增迁移修改，禁止重写历史。

CREATE TABLE schema_migrations (
    version INTEGER NOT NULL PRIMARY KEY CHECK (typeof(version) = 'integer' AND version > 0),
    name TEXT NOT NULL CHECK (typeof(name) = 'text'),
    definition TEXT NOT NULL CHECK (typeof(definition) = 'text'),
    applied_at INTEGER NOT NULL CHECK (typeof(applied_at) = 'integer')
);

CREATE TABLE users (
    id TEXT COLLATE BINARY NOT NULL PRIMARY KEY CHECK (
        typeof(id) = 'text'
        AND length(CAST(id AS BLOB)) = 36
        AND substr(id, 9, 1) = '-'
        AND substr(id, 14, 1) = '-'
        AND substr(id, 19, 1) = '-'
        AND substr(id, 24, 1) = '-'
        AND length(replace(id, '-', '')) = 32
        AND replace(id, '-', '') NOT GLOB '*[^0-9a-f]*'
        AND id <> '00000000-0000-0000-0000-000000000000'),
    role INTEGER NOT NULL CHECK (typeof(role) = 'integer' AND role IN (0, 1)),
    enabled INTEGER NOT NULL CHECK (typeof(enabled) = 'integer' AND enabled IN (0, 1)),
    auth_epoch INTEGER NOT NULL CHECK (typeof(auth_epoch) = 'integer' AND auth_epoch >= 0)
);

CREATE UNIQUE INDEX users_one_administrator ON users(role) WHERE role = 1;

CREATE TABLE sessions (
    token_hash BLOB NOT NULL CHECK (typeof(token_hash) = 'blob' AND length(token_hash) = 32) PRIMARY KEY,
    user_id TEXT COLLATE BINARY NOT NULL REFERENCES users(id) CHECK (
        typeof(user_id) = 'text'
        AND length(CAST(user_id AS BLOB)) = 36
        AND substr(user_id, 9, 1) = '-'
        AND substr(user_id, 14, 1) = '-'
        AND substr(user_id, 19, 1) = '-'
        AND substr(user_id, 24, 1) = '-'
        AND length(replace(user_id, '-', '')) = 32
        AND replace(user_id, '-', '') NOT GLOB '*[^0-9a-f]*'
        AND user_id <> '00000000-0000-0000-0000-000000000000'),
    auth_epoch INTEGER NOT NULL CHECK (typeof(auth_epoch) = 'integer' AND auth_epoch >= 0),
    expires_at INTEGER NOT NULL CHECK (typeof(expires_at) = 'integer'),
    revoked INTEGER NOT NULL CHECK (typeof(revoked) = 'integer' AND revoked IN (0, 1))
);

CREATE INDEX sessions_user ON sessions(user_id);

CREATE TABLE spaces (
    id TEXT COLLATE BINARY NOT NULL PRIMARY KEY CHECK (
        typeof(id) = 'text'
        AND length(CAST(id AS BLOB)) = 36
        AND substr(id, 9, 1) = '-'
        AND substr(id, 14, 1) = '-'
        AND substr(id, 19, 1) = '-'
        AND substr(id, 24, 1) = '-'
        AND length(replace(id, '-', '')) = 32
        AND replace(id, '-', '') NOT GLOB '*[^0-9a-f]*'
        AND id <> '00000000-0000-0000-0000-000000000000'),
    kind INTEGER NOT NULL CHECK (typeof(kind) = 'integer' AND kind IN (0, 1)),
    owner_id TEXT COLLATE BINARY REFERENCES users(id) CHECK (
        owner_id IS NULL OR (typeof(owner_id) = 'text'
        AND length(CAST(owner_id AS BLOB)) = 36
        AND substr(owner_id, 9, 1) = '-'
        AND substr(owner_id, 14, 1) = '-'
        AND substr(owner_id, 19, 1) = '-'
        AND substr(owner_id, 24, 1) = '-'
        AND length(replace(owner_id, '-', '')) = 32
        AND replace(owner_id, '-', '') NOT GLOB '*[^0-9a-f]*'
        AND owner_id <> '00000000-0000-0000-0000-000000000000')),
    root_node_id TEXT COLLATE BINARY NOT NULL CHECK (
        typeof(root_node_id) = 'text'
        AND length(CAST(root_node_id AS BLOB)) = 36
        AND substr(root_node_id, 9, 1) = '-'
        AND substr(root_node_id, 14, 1) = '-'
        AND substr(root_node_id, 19, 1) = '-'
        AND substr(root_node_id, 24, 1) = '-'
        AND length(replace(root_node_id, '-', '')) = 32
        AND replace(root_node_id, '-', '') NOT GLOB '*[^0-9a-f]*'
        AND root_node_id <> '00000000-0000-0000-0000-000000000000'),
    CHECK ((kind = 0 AND owner_id IS NOT NULL) OR (kind = 1 AND owner_id IS NULL)),
    FOREIGN KEY (id, root_node_id) REFERENCES nodes(space_id, id) DEFERRABLE INITIALLY DEFERRED
);

CREATE UNIQUE INDEX spaces_private_owner ON spaces(owner_id) WHERE kind = 0;
CREATE UNIQUE INDEX spaces_one_public ON spaces(kind) WHERE kind = 1;

CREATE TABLE blobs (
    id TEXT COLLATE BINARY NOT NULL PRIMARY KEY CHECK (
        typeof(id) = 'text'
        AND length(CAST(id AS BLOB)) = 36
        AND substr(id, 9, 1) = '-'
        AND substr(id, 14, 1) = '-'
        AND substr(id, 19, 1) = '-'
        AND substr(id, 24, 1) = '-'
        AND length(replace(id, '-', '')) = 32
        AND replace(id, '-', '') NOT GLOB '*[^0-9a-f]*'
        AND id <> '00000000-0000-0000-0000-000000000000'),
    size INTEGER NOT NULL CHECK (typeof(size) = 'integer' AND size >= 0),
    sha256 BLOB NOT NULL CHECK (typeof(sha256) = 'blob' AND length(sha256) = 32)
);

CREATE TABLE nodes (
    id TEXT COLLATE BINARY NOT NULL PRIMARY KEY CHECK (
        typeof(id) = 'text'
        AND length(CAST(id AS BLOB)) = 36
        AND substr(id, 9, 1) = '-'
        AND substr(id, 14, 1) = '-'
        AND substr(id, 19, 1) = '-'
        AND substr(id, 24, 1) = '-'
        AND length(replace(id, '-', '')) = 32
        AND replace(id, '-', '') NOT GLOB '*[^0-9a-f]*'
        AND id <> '00000000-0000-0000-0000-000000000000'),
    space_id TEXT COLLATE BINARY NOT NULL REFERENCES spaces(id) DEFERRABLE INITIALLY DEFERRED CHECK (
        typeof(space_id) = 'text'
        AND length(CAST(space_id AS BLOB)) = 36
        AND substr(space_id, 9, 1) = '-'
        AND substr(space_id, 14, 1) = '-'
        AND substr(space_id, 19, 1) = '-'
        AND substr(space_id, 24, 1) = '-'
        AND length(replace(space_id, '-', '')) = 32
        AND replace(space_id, '-', '') NOT GLOB '*[^0-9a-f]*'
        AND space_id <> '00000000-0000-0000-0000-000000000000'),
    parent_id TEXT COLLATE BINARY CHECK (
        parent_id IS NULL OR (typeof(parent_id) = 'text'
        AND length(CAST(parent_id AS BLOB)) = 36
        AND substr(parent_id, 9, 1) = '-'
        AND substr(parent_id, 14, 1) = '-'
        AND substr(parent_id, 19, 1) = '-'
        AND substr(parent_id, 24, 1) = '-'
        AND length(replace(parent_id, '-', '')) = 32
        AND replace(parent_id, '-', '') NOT GLOB '*[^0-9a-f]*'
        AND parent_id <> '00000000-0000-0000-0000-000000000000')),
    name TEXT COLLATE BINARY NOT NULL CHECK (typeof(name) = 'text'),
    kind INTEGER NOT NULL CHECK (typeof(kind) = 'integer' AND kind IN (0, 1)),
    created_by TEXT COLLATE BINARY NOT NULL REFERENCES users(id) CHECK (
        typeof(created_by) = 'text'
        AND length(CAST(created_by AS BLOB)) = 36
        AND substr(created_by, 9, 1) = '-'
        AND substr(created_by, 14, 1) = '-'
        AND substr(created_by, 19, 1) = '-'
        AND substr(created_by, 24, 1) = '-'
        AND length(replace(created_by, '-', '')) = 32
        AND replace(created_by, '-', '') NOT GLOB '*[^0-9a-f]*'
        AND created_by <> '00000000-0000-0000-0000-000000000000'),
    blob_id TEXT COLLATE BINARY REFERENCES blobs(id) CHECK (
        blob_id IS NULL OR (typeof(blob_id) = 'text'
        AND length(CAST(blob_id AS BLOB)) = 36
        AND substr(blob_id, 9, 1) = '-'
        AND substr(blob_id, 14, 1) = '-'
        AND substr(blob_id, 19, 1) = '-'
        AND substr(blob_id, 24, 1) = '-'
        AND length(replace(blob_id, '-', '')) = 32
        AND replace(blob_id, '-', '') NOT GLOB '*[^0-9a-f]*'
        AND blob_id <> '00000000-0000-0000-0000-000000000000')),
    revision INTEGER NOT NULL CHECK (typeof(revision) = 'integer' AND revision >= 1),
    trashed INTEGER NOT NULL CHECK (typeof(trashed) = 'integer' AND trashed IN (0, 1)),
    created_at INTEGER NOT NULL CHECK (typeof(created_at) = 'integer'),
    updated_at INTEGER NOT NULL CHECK (typeof(updated_at) = 'integer'),
    UNIQUE (space_id, id),
    FOREIGN KEY (space_id, parent_id) REFERENCES nodes(space_id, id),
    CHECK (parent_id IS NULL OR parent_id <> id),
    CHECK ((kind = 0 AND blob_id IS NULL) OR (kind = 1 AND blob_id IS NOT NULL)),
    CHECK ((name = '' AND parent_id IS NULL AND kind = 0 AND trashed = 0) OR
           (length(CAST(name AS BLOB)) BETWEEN 1 AND 255 AND instr(name, char(0)) = 0
            AND instr(name, '/') = 0 AND instr(name, char(92)) = 0 AND name NOT IN ('.', '..')))
);

CREATE UNIQUE INDEX nodes_active_name ON nodes(space_id, parent_id, name COLLATE BINARY) WHERE trashed = 0 AND parent_id IS NOT NULL;
CREATE UNIQUE INDEX nodes_active_root ON nodes(space_id) WHERE trashed = 0 AND parent_id IS NULL;
CREATE INDEX nodes_blob ON nodes(blob_id);

-- 根必须与空间声明一致；回收站祖先可有未标记后代，完整活动性由仓储遍历。
CREATE TRIGGER nodes_shape_insert BEFORE INSERT ON nodes BEGIN
    SELECT RAISE(ABORT, 'invalid root') WHERE
        (NEW.id = (SELECT root_node_id FROM spaces WHERE id = NEW.space_id)
         AND (NEW.parent_id IS NOT NULL OR NEW.kind <> 0 OR NEW.trashed <> 0 OR NEW.name <> ''))
        OR (NEW.trashed = 0 AND NEW.parent_id IS NULL
            AND NOT EXISTS (SELECT 1 FROM spaces WHERE id = NEW.space_id AND root_node_id = NEW.id));
    SELECT RAISE(ABORT, 'parent is not a directory') WHERE NEW.parent_id IS NOT NULL
        AND EXISTS (SELECT 1 FROM nodes WHERE id = NEW.parent_id AND space_id = NEW.space_id AND kind <> 0);
END;

-- 根必须与空间声明一致；回收站祖先可有未标记后代，完整活动性由仓储遍历。
CREATE TRIGGER nodes_shape_update BEFORE UPDATE ON nodes BEGIN
    SELECT RAISE(ABORT, 'invalid root') WHERE
        (NEW.id = (SELECT root_node_id FROM spaces WHERE id = NEW.space_id)
         AND (NEW.parent_id IS NOT NULL OR NEW.kind <> 0 OR NEW.trashed <> 0 OR NEW.name <> ''))
        OR (NEW.trashed = 0 AND NEW.parent_id IS NULL
            AND NOT EXISTS (SELECT 1 FROM spaces WHERE id = NEW.space_id AND root_node_id = NEW.id));
    SELECT RAISE(ABORT, 'parent is not a directory') WHERE NEW.parent_id IS NOT NULL
        AND EXISTS (SELECT 1 FROM nodes WHERE id = NEW.parent_id AND space_id = NEW.space_id AND kind <> 0);
END;

CREATE TRIGGER nodes_identity_update BEFORE UPDATE OF id, space_id, kind ON nodes
WHEN NEW.id <> OLD.id OR NEW.space_id <> OLD.space_id OR NEW.kind <> OLD.kind BEGIN
    SELECT RAISE(ABORT, 'node identity is immutable');
END;

CREATE TRIGGER nodes_root_delete BEFORE DELETE ON nodes
WHEN EXISTS (SELECT 1 FROM spaces WHERE id = OLD.space_id AND root_node_id = OLD.id) BEGIN
    SELECT RAISE(ABORT, 'root is immutable');
END;

CREATE TRIGGER spaces_identity_update BEFORE UPDATE OF id, kind, owner_id, root_node_id ON spaces
WHEN NEW.id <> OLD.id OR NEW.kind <> OLD.kind OR NEW.owner_id IS NOT OLD.owner_id OR NEW.root_node_id <> OLD.root_node_id BEGIN
    SELECT RAISE(ABORT, 'space identity is immutable');
END;

CREATE TRIGGER spaces_root_insert BEFORE INSERT ON spaces
WHEN EXISTS (SELECT 1 FROM nodes WHERE id = NEW.root_node_id
    AND (space_id <> NEW.id OR parent_id IS NOT NULL OR kind <> 0 OR trashed <> 0 OR name <> '')) BEGIN
    SELECT RAISE(ABORT, 'invalid space root');
END;

CREATE TABLE space_quotas (
    space_id TEXT COLLATE BINARY NOT NULL REFERENCES spaces(id) CHECK (
        typeof(space_id) = 'text'
        AND length(CAST(space_id AS BLOB)) = 36
        AND substr(space_id, 9, 1) = '-'
        AND substr(space_id, 14, 1) = '-'
        AND substr(space_id, 19, 1) = '-'
        AND substr(space_id, 24, 1) = '-'
        AND length(replace(space_id, '-', '')) = 32
        AND replace(space_id, '-', '') NOT GLOB '*[^0-9a-f]*'
        AND space_id <> '00000000-0000-0000-0000-000000000000'),
    limit_bytes INTEGER NOT NULL CHECK (typeof(limit_bytes) = 'integer' AND limit_bytes >= 0),
    limit_nodes INTEGER NOT NULL CHECK (typeof(limit_nodes) = 'integer' AND limit_nodes >= 0),
    used_bytes INTEGER NOT NULL CHECK (typeof(used_bytes) = 'integer' AND used_bytes >= 0),
    used_nodes INTEGER NOT NULL CHECK (typeof(used_nodes) = 'integer' AND used_nodes >= 0),
    reserved_bytes INTEGER NOT NULL CHECK (typeof(reserved_bytes) = 'integer' AND reserved_bytes >= 0),
    reserved_nodes INTEGER NOT NULL CHECK (typeof(reserved_nodes) = 'integer' AND reserved_nodes >= 0),
    PRIMARY KEY (space_id),
    CHECK (used_bytes <= limit_bytes AND reserved_bytes <= limit_bytes - used_bytes),
    CHECK (used_nodes <= limit_nodes AND reserved_nodes <= limit_nodes - used_nodes)
);

CREATE TABLE public_contribution_quotas (
    space_id TEXT COLLATE BINARY NOT NULL REFERENCES spaces(id) CHECK (
        typeof(space_id) = 'text'
        AND length(CAST(space_id AS BLOB)) = 36
        AND substr(space_id, 9, 1) = '-'
        AND substr(space_id, 14, 1) = '-'
        AND substr(space_id, 19, 1) = '-'
        AND substr(space_id, 24, 1) = '-'
        AND length(replace(space_id, '-', '')) = 32
        AND replace(space_id, '-', '') NOT GLOB '*[^0-9a-f]*'
        AND space_id <> '00000000-0000-0000-0000-000000000000'),
    user_id TEXT COLLATE BINARY NOT NULL REFERENCES users(id) CHECK (
        typeof(user_id) = 'text'
        AND length(CAST(user_id AS BLOB)) = 36
        AND substr(user_id, 9, 1) = '-'
        AND substr(user_id, 14, 1) = '-'
        AND substr(user_id, 19, 1) = '-'
        AND substr(user_id, 24, 1) = '-'
        AND length(replace(user_id, '-', '')) = 32
        AND replace(user_id, '-', '') NOT GLOB '*[^0-9a-f]*'
        AND user_id <> '00000000-0000-0000-0000-000000000000'),
    limit_bytes INTEGER NOT NULL CHECK (typeof(limit_bytes) = 'integer' AND limit_bytes >= 0),
    limit_nodes INTEGER NOT NULL CHECK (typeof(limit_nodes) = 'integer' AND limit_nodes >= 0),
    used_bytes INTEGER NOT NULL CHECK (typeof(used_bytes) = 'integer' AND used_bytes >= 0),
    used_nodes INTEGER NOT NULL CHECK (typeof(used_nodes) = 'integer' AND used_nodes >= 0),
    reserved_bytes INTEGER NOT NULL CHECK (typeof(reserved_bytes) = 'integer' AND reserved_bytes >= 0),
    reserved_nodes INTEGER NOT NULL CHECK (typeof(reserved_nodes) = 'integer' AND reserved_nodes >= 0),
    PRIMARY KEY (space_id, user_id),
    CHECK (used_bytes <= limit_bytes AND reserved_bytes <= limit_bytes - used_bytes),
    CHECK (used_nodes <= limit_nodes AND reserved_nodes <= limit_nodes - used_nodes)
);

CREATE TRIGGER public_contribution_quotas_insert BEFORE INSERT ON public_contribution_quotas
WHEN NOT EXISTS (SELECT 1 FROM spaces WHERE id = NEW.space_id AND kind = 1) BEGIN
    SELECT RAISE(ABORT, 'contribution requires public space');
END;

CREATE TRIGGER public_contribution_quotas_update BEFORE UPDATE ON public_contribution_quotas
WHEN NOT EXISTS (SELECT 1 FROM spaces WHERE id = NEW.space_id AND kind = 1) BEGIN
    SELECT RAISE(ABORT, 'contribution requires public space');
END;

CREATE TABLE audit_events (
    sequence INTEGER NOT NULL PRIMARY KEY CHECK (typeof(sequence) = 'integer' AND sequence > 0),
    actor_id TEXT COLLATE BINARY NOT NULL REFERENCES users(id) CHECK (
        typeof(actor_id) = 'text'
        AND length(CAST(actor_id AS BLOB)) = 36
        AND substr(actor_id, 9, 1) = '-'
        AND substr(actor_id, 14, 1) = '-'
        AND substr(actor_id, 19, 1) = '-'
        AND substr(actor_id, 24, 1) = '-'
        AND length(replace(actor_id, '-', '')) = 32
        AND replace(actor_id, '-', '') NOT GLOB '*[^0-9a-f]*'
        AND actor_id <> '00000000-0000-0000-0000-000000000000'),
    space_id TEXT COLLATE BINARY NOT NULL REFERENCES spaces(id) CHECK (
        typeof(space_id) = 'text'
        AND length(CAST(space_id AS BLOB)) = 36
        AND substr(space_id, 9, 1) = '-'
        AND substr(space_id, 14, 1) = '-'
        AND substr(space_id, 19, 1) = '-'
        AND substr(space_id, 24, 1) = '-'
        AND length(replace(space_id, '-', '')) = 32
        AND replace(space_id, '-', '') NOT GLOB '*[^0-9a-f]*'
        AND space_id <> '00000000-0000-0000-0000-000000000000'),
    node_id TEXT COLLATE BINARY NOT NULL CHECK (
        typeof(node_id) = 'text'
        AND length(CAST(node_id AS BLOB)) = 36
        AND substr(node_id, 9, 1) = '-'
        AND substr(node_id, 14, 1) = '-'
        AND substr(node_id, 19, 1) = '-'
        AND substr(node_id, 24, 1) = '-'
        AND length(replace(node_id, '-', '')) = 32
        AND replace(node_id, '-', '') NOT GLOB '*[^0-9a-f]*'
        AND node_id <> '00000000-0000-0000-0000-000000000000'),
    parent_id TEXT COLLATE BINARY NOT NULL CHECK (
        typeof(parent_id) = 'text'
        AND length(CAST(parent_id AS BLOB)) = 36
        AND substr(parent_id, 9, 1) = '-'
        AND substr(parent_id, 14, 1) = '-'
        AND substr(parent_id, 19, 1) = '-'
        AND substr(parent_id, 24, 1) = '-'
        AND length(replace(parent_id, '-', '')) = 32
        AND replace(parent_id, '-', '') NOT GLOB '*[^0-9a-f]*'
        AND parent_id <> '00000000-0000-0000-0000-000000000000'),
    request_id TEXT COLLATE BINARY NOT NULL CHECK (
        typeof(request_id) = 'text'
        AND length(CAST(request_id AS BLOB)) = 36
        AND substr(request_id, 9, 1) = '-'
        AND substr(request_id, 14, 1) = '-'
        AND substr(request_id, 19, 1) = '-'
        AND substr(request_id, 24, 1) = '-'
        AND length(replace(request_id, '-', '')) = 32
        AND replace(request_id, '-', '') NOT GLOB '*[^0-9a-f]*'
        AND request_id <> '00000000-0000-0000-0000-000000000000'),
    action TEXT NOT NULL CHECK (typeof(action) = 'text' AND action = 'directory_created'),
    occurred_at INTEGER NOT NULL CHECK (typeof(occurred_at) = 'integer')
);

