-- 当前 schema 3；用于新实例建库，无开发版本升级链。
CREATE TABLE schema_info (
    singleton INTEGER NOT NULL PRIMARY KEY CHECK (typeof(singleton)='integer' AND singleton=1),
    version INTEGER NOT NULL CHECK (typeof(version)='integer' AND version>0),
    definition TEXT NOT NULL CHECK (typeof(definition)='text'),
    created_at INTEGER NOT NULL CHECK (typeof(created_at)='integer')
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
    auth_epoch INTEGER NOT NULL CHECK (typeof(auth_epoch) = 'integer' AND auth_epoch >= 0),
    login TEXT COLLATE BINARY NOT NULL UNIQUE CHECK (typeof(login)='text' AND anof_login(login)),
    display_name TEXT NOT NULL CHECK (typeof(display_name)='text' AND anof_display_name(display_name)),
    password_hash TEXT NOT NULL CHECK (typeof(password_hash)='text' AND anof_password_hash(password_hash)),
    password_version INTEGER NOT NULL CHECK (typeof(password_version)='integer' AND password_version>=1),
    revision INTEGER NOT NULL CHECK (typeof(revision)='integer' AND revision>=1),
    created_at INTEGER NOT NULL CHECK (typeof(created_at)='integer'),
    updated_at INTEGER NOT NULL CHECK (typeof(updated_at)='integer'),
    CHECK (role<>1 OR enabled=1)
);

CREATE UNIQUE INDEX users_one_administrator ON users(role) WHERE role = 1;

CREATE TRIGGER users_identity_update BEFORE UPDATE OF id, login, role ON users
WHEN NEW.id<>OLD.id OR NEW.login<>OLD.login OR NEW.role<>OLD.role BEGIN
    SELECT RAISE(ABORT, 'user identity is immutable');
END;
CREATE TRIGGER users_delete BEFORE DELETE ON users BEGIN
    SELECT RAISE(ABORT, 'user deletion is unsupported');
END;

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
    revoked INTEGER NOT NULL CHECK (typeof(revoked) = 'integer' AND revoked IN (0, 1)),
    created_at INTEGER NOT NULL CHECK (typeof(created_at)='integer'),
    credential_mode TEXT NOT NULL CHECK (typeof(credential_mode)='text' AND credential_mode='bearer'),
    CHECK (expires_at>created_at)
);

CREATE INDEX sessions_user ON sessions(user_id);
CREATE INDEX sessions_expiry ON sessions(expires_at, token_hash);

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
    size INTEGER NOT NULL CHECK (typeof(size) = 'integer' AND size >= 0 AND size <= 1099511627776),
    sha256 BLOB NOT NULL CHECK (typeof(sha256) = 'blob' AND length(sha256) = 32),
    storage_key TEXT GENERATED ALWAYS AS ('objects/' || substr(id,1,2) || '/' || id) VIRTUAL,
    state INTEGER NOT NULL DEFAULT 0 CHECK(state=0)
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
CREATE INDEX nodes_children ON nodes(space_id, parent_id, name COLLATE BINARY, id) WHERE trashed=0;

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
    revision INTEGER NOT NULL CHECK (typeof(revision)='integer' AND revision>=1),
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
    revision INTEGER NOT NULL CHECK (typeof(revision)='integer' AND revision>=1),
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


CREATE TABLE invites (
    id TEXT COLLATE BINARY NOT NULL PRIMARY KEY CHECK (anof_uuid(id)),
    token_hash BLOB NOT NULL UNIQUE CHECK (typeof(token_hash)='blob' AND length(token_hash)=32),
    created_by TEXT COLLATE BINARY NOT NULL REFERENCES users(id),
    created_at INTEGER NOT NULL CHECK (typeof(created_at)='integer'),
    expires_at INTEGER NOT NULL CHECK (typeof(expires_at)='integer'),
    max_uses INTEGER NOT NULL CHECK (typeof(max_uses)='integer' AND max_uses BETWEEN 1 AND 10000),
    used_count INTEGER NOT NULL CHECK (typeof(used_count)='integer' AND used_count BETWEEN 0 AND max_uses),
    revoked_at INTEGER CHECK (revoked_at IS NULL OR typeof(revoked_at)='integer'),
    CHECK (expires_at>created_at AND
        (created_at>9223372036854775807-2592000000000 OR expires_at<=created_at+2592000000000))
);
CREATE INDEX invites_created ON invites(created_at, id);
CREATE TRIGGER invites_creator_insert BEFORE INSERT ON invites
WHEN NOT EXISTS (SELECT 1 FROM users WHERE id=NEW.created_by AND role=1 AND enabled=1) BEGIN
    SELECT RAISE(ABORT, 'invite requires administrator');
END;
CREATE TRIGGER invites_identity_update BEFORE UPDATE ON invites
WHEN NEW.id<>OLD.id OR NEW.token_hash<>OLD.token_hash OR NEW.created_by<>OLD.created_by
    OR NEW.created_at<>OLD.created_at OR NEW.expires_at<>OLD.expires_at OR NEW.max_uses<>OLD.max_uses
    OR NEW.used_count<OLD.used_count OR (OLD.revoked_at IS NOT NULL AND NEW.revoked_at IS NOT OLD.revoked_at) BEGIN
    SELECT RAISE(ABORT, 'invite identity is immutable');
END;

CREATE TABLE instance_settings (
    singleton INTEGER NOT NULL PRIMARY KEY CHECK (typeof(singleton)='integer' AND singleton=1),
    instance_id TEXT COLLATE BINARY NOT NULL UNIQUE CHECK (anof_uuid(instance_id)),
    bootstrap_operation_id TEXT COLLATE BINARY NOT NULL UNIQUE CHECK (anof_uuid(bootstrap_operation_id)),
    initialized_at INTEGER NOT NULL CHECK (typeof(initialized_at)='integer'),
    max_users INTEGER NOT NULL CHECK (typeof(max_users)='integer' AND max_users BETWEEN 1 AND 10000),
    default_private_bytes INTEGER NOT NULL CHECK (typeof(default_private_bytes)='integer' AND default_private_bytes>=0),
    default_private_nodes INTEGER NOT NULL CHECK (typeof(default_private_nodes)='integer' AND default_private_nodes>=1),
    default_contribution_bytes INTEGER NOT NULL CHECK (typeof(default_contribution_bytes)='integer' AND default_contribution_bytes>=0),
    default_contribution_nodes INTEGER NOT NULL CHECK (typeof(default_contribution_nodes)='integer' AND default_contribution_nodes>=1),
    revision INTEGER NOT NULL CHECK (typeof(revision)='integer' AND revision>=1)
);
CREATE TRIGGER instance_settings_identity_update BEFORE UPDATE OF singleton,instance_id,bootstrap_operation_id,initialized_at ON instance_settings
WHEN NEW.singleton<>OLD.singleton OR NEW.instance_id<>OLD.instance_id OR NEW.bootstrap_operation_id<>OLD.bootstrap_operation_id
    OR NEW.initialized_at<>OLD.initialized_at BEGIN
    SELECT RAISE(ABORT, 'initialization identity is immutable');
END;

CREATE TABLE identity_audit_events (
    sequence INTEGER NOT NULL PRIMARY KEY CHECK (typeof(sequence)='integer' AND sequence>0),
    actor_id TEXT COLLATE BINARY REFERENCES users(id),
    subject_user_id TEXT COLLATE BINARY REFERENCES users(id),
    invite_id TEXT COLLATE BINARY REFERENCES invites(id),
    instance_id TEXT COLLATE BINARY REFERENCES instance_settings(instance_id),
    space_id TEXT COLLATE BINARY REFERENCES spaces(id),
    user_id TEXT COLLATE BINARY REFERENCES users(id),
    quota_scope TEXT CHECK (quota_scope IS NULL OR quota_scope IN ('space','contribution')),
    request_id TEXT COLLATE BINARY NOT NULL CHECK (anof_uuid(request_id)),
    action TEXT NOT NULL CHECK (typeof(action)='text'),
    occurred_at INTEGER NOT NULL CHECK (typeof(occurred_at)='integer'),
    CHECK (
        (action='instance_initialized' AND actor_id IS NULL AND subject_user_id IS NOT NULL AND instance_id IS NOT NULL AND invite_id IS NULL AND space_id IS NULL AND user_id IS NULL AND quota_scope IS NULL)
        OR (action IN ('invite_created','invite_revoked') AND actor_id IS NOT NULL AND invite_id IS NOT NULL AND subject_user_id IS NULL AND instance_id IS NULL AND space_id IS NULL AND user_id IS NULL AND quota_scope IS NULL)
        OR (action='user_registered' AND actor_id IS NULL AND subject_user_id IS NOT NULL AND invite_id IS NOT NULL AND instance_id IS NULL AND space_id IS NULL AND user_id IS NULL AND quota_scope IS NULL)
        OR (action IN ('session_created','session_revoked','password_changed','display_name_changed') AND actor_id IS NOT NULL AND actor_id=subject_user_id AND subject_user_id IS NOT NULL AND invite_id IS NULL AND instance_id IS NULL AND space_id IS NULL AND user_id IS NULL AND quota_scope IS NULL)
        OR (action IN ('user_enabled','user_disabled') AND actor_id IS NOT NULL AND subject_user_id IS NOT NULL AND invite_id IS NULL AND instance_id IS NULL AND space_id IS NULL AND user_id IS NULL AND quota_scope IS NULL)
        OR (action='quota_changed' AND actor_id IS NOT NULL AND subject_user_id IS NULL AND invite_id IS NULL AND instance_id IS NULL AND space_id IS NOT NULL AND quota_scope IS NOT NULL
            AND ((quota_scope='space' AND user_id IS NULL) OR (quota_scope='contribution' AND user_id IS NOT NULL)))
    )
);

CREATE TRIGGER blobs_immutable BEFORE UPDATE ON blobs BEGIN SELECT RAISE(ABORT,'immutable blob'); END;
CREATE TRIGGER blobs_no_delete BEFORE DELETE ON blobs BEGIN SELECT RAISE(ABORT,'no blob gc'); END;
CREATE TABLE storage_accounting (
 singleton INTEGER PRIMARY KEY CHECK(singleton=1),
 content_budget_bytes INTEGER NOT NULL CHECK(typeof(content_budget_bytes)='integer' AND content_budget_bytes>=0),
 retained_bytes INTEGER NOT NULL CHECK(typeof(retained_bytes)='integer' AND retained_bytes>=0),
 reserved_bytes INTEGER NOT NULL CHECK(typeof(reserved_bytes)='integer' AND reserved_bytes>=0 AND reserved_bytes<=9223372036854775807-retained_bytes),
 revision INTEGER NOT NULL CHECK(typeof(revision)='integer' AND revision>=1)
);
CREATE TABLE uploads (
 id TEXT PRIMARY KEY CHECK(anof_uuid(id)),
 user_id TEXT NOT NULL REFERENCES users(id),
 client_upload_id TEXT NOT NULL CHECK(anof_uuid(client_upload_id)),
 space_id TEXT NOT NULL REFERENCES spaces(id),
 original_parent_id TEXT NOT NULL CHECK(anof_uuid(original_parent_id)),
 original_name TEXT NOT NULL CHECK(anof_node_name(original_name)),
 length INTEGER NOT NULL CHECK(typeof(length)='integer' AND length BETWEEN 0 AND 1099511627776),
 expected_sha256 TEXT CHECK(expected_sha256 IS NULL OR (length(expected_sha256)=64 AND expected_sha256 NOT GLOB '*[^0-9a-f]*')),
 parent_id TEXT NOT NULL CHECK(anof_uuid(parent_id)),
 name TEXT NOT NULL CHECK(anof_node_name(name)),
 durable_offset INTEGER NOT NULL CHECK(typeof(durable_offset)='integer' AND durable_offset BETWEEN 0 AND length),
 state INTEGER NOT NULL CHECK(typeof(state)='integer' AND state BETWEEN 0 AND 8),
 generation INTEGER NOT NULL CHECK(typeof(generation)='integer' AND generation>=1),
 actual_sha256 TEXT CHECK(actual_sha256 IS NULL OR (length(actual_sha256)=64 AND actual_sha256 NOT GLOB '*[^0-9a-f]*')),
 pending_blob_id TEXT CHECK(pending_blob_id IS NULL OR anof_uuid(pending_blob_id)),
 staging_location INTEGER NOT NULL CHECK(staging_location IN (0,1)),
 result_node_id TEXT CHECK(result_node_id IS NULL OR anof_uuid(result_node_id)),
 created_at INTEGER NOT NULL CHECK(typeof(created_at)='integer'),
 last_progress_at INTEGER NOT NULL CHECK(typeof(last_progress_at)='integer' AND last_progress_at>=created_at),
 expires_at INTEGER NOT NULL CHECK(typeof(expires_at)='integer' AND expires_at>=last_progress_at),
 terminal_at INTEGER CHECK(terminal_at IS NULL OR (typeof(terminal_at)='integer' AND terminal_at>=last_progress_at)),
 last_error INTEGER CHECK(last_error IS NULL OR (typeof(last_error)='integer' AND last_error BETWEEN 0 AND 11)),
 cancel_requested INTEGER NOT NULL CHECK(cancel_requested IN (0,1)),
 cleanup_pending INTEGER NOT NULL CHECK(cleanup_pending IN (0,1)),
 UNIQUE(user_id,client_upload_id),
 CHECK((state>=5)=(terminal_at IS NOT NULL)),
 CHECK((state=5)=(result_node_id IS NOT NULL)),
 CHECK(cleanup_pending=0 OR state IN (6,7,8)),
 CHECK(state NOT IN (2,3,4,5) OR durable_offset=length),
 CHECK(state!=0 OR durable_offset=0),
 CHECK(state NOT IN (3,4,5) OR actual_sha256 IS NOT NULL),
 CHECK((staging_location=0 AND state NOT IN (4,5)) OR pending_blob_id IS NOT NULL),
 CHECK(state!=5 OR (staging_location=1 AND cancel_requested=0 AND last_error IS NULL))
);
CREATE INDEX uploads_user_seek ON uploads(user_id,created_at,id);
CREATE INDEX uploads_seek ON uploads(created_at,id);
CREATE INDEX uploads_candidate ON uploads(pending_blob_id,state);
CREATE TRIGGER upload_original_immutable BEFORE UPDATE ON uploads
WHEN NEW.id!=OLD.id OR NEW.user_id!=OLD.user_id OR NEW.client_upload_id!=OLD.client_upload_id OR
 NEW.space_id!=OLD.space_id OR NEW.original_parent_id!=OLD.original_parent_id OR NEW.original_name!=OLD.original_name OR
 NEW.length!=OLD.length OR NEW.expected_sha256 IS NOT OLD.expected_sha256 OR NEW.created_at!=OLD.created_at OR
 NEW.generation!=OLD.generation+1 OR NEW.durable_offset<OLD.durable_offset OR NEW.cancel_requested<OLD.cancel_requested
BEGIN SELECT RAISE(ABORT,'upload invariant'); END;
CREATE TABLE transfer_audit_events (
 sequence INTEGER PRIMARY KEY,
 kind INTEGER NOT NULL CHECK(kind BETWEEN 0 AND 6),
 request_id TEXT NOT NULL CHECK(anof_uuid(request_id)),
 actor_id TEXT REFERENCES users(id),
 upload_id TEXT CHECK(upload_id IS NULL OR anof_uuid(upload_id)),
 occurred_at INTEGER NOT NULL CHECK(typeof(occurred_at)='integer'),
 failure INTEGER CHECK(failure IS NULL OR failure BETWEEN 0 AND 11),
 CHECK((kind=6 AND actor_id IS NOT NULL AND upload_id IS NULL AND failure IS NULL) OR (kind!=6 AND upload_id IS NOT NULL))
);
