-- Exécutions d'oracles ASYNCHRONES.
-- Quand aret_run_oracle est lancé en mode async (les oracles lourds — winediff,
-- difftest, cpudiff — dépassent le timeout transport MCP de 60s), le serveur
-- exécute l'oracle dans un thread de fond et enregistre ICI un suivi DURABLE :
-- RUNNING au lancement, puis DONE/ERROR à la fin avec le proof lié. aret_get_oracle_run
-- interroge ce suivi. Il SURVIT à la compaction du contexte et au redémarrage serveur
-- (une exécution laissée RUNNING par un crash reste visible et diagnosticable).
-- Les artefacts et la preuve restent dans les tables proof/artifacts existantes ;
-- cette table ne porte que l'identité, l'état et le pointeur vers le proof.

CREATE TABLE IF NOT EXISTS oracle_run (
    id TEXT PRIMARY KEY,
    oracle TEXT NOT NULL,
    knowledge_id TEXT,
    promote INTEGER NOT NULL DEFAULT 0 CHECK(promote IN (0,1)),
    status TEXT NOT NULL CHECK(status IN ('RUNNING','DONE','ERROR')),
    result TEXT CHECK(result IN ('PASS','FAIL','ERROR','SKIPPED','UNKNOWN') OR result IS NULL),
    proof_id TEXT,
    exit_code INTEGER,
    error TEXT,
    started_at TEXT NOT NULL,
    finished_at TEXT,
    created_at TEXT NOT NULL,
    created_by TEXT NOT NULL
) STRICT;

CREATE INDEX IF NOT EXISTS idx_oracle_run_status_created ON oracle_run(status, created_at DESC, id);

INSERT OR IGNORE INTO id_sequence(entity, next_value) VALUES ('oracle_run', 1);
