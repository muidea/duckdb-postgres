#pragma once
// Kept byte-identical to the source descriptor query in internal/mutation.
namespace duckdb {
static const char *FQE_POSTGRES_METADATA_SQL = R"FQEMETA(SELECT pg_catalog.left(pg_catalog.json_build_object(
 'database', pg_catalog.current_database(),
 'database_oid', (SELECT d.oid::pg_catalog.int8 FROM pg_catalog.pg_database d WHERE d.datname=pg_catalog.current_database()),
 'schema', n.nspname, 'table', c.relname,
 'relation_oid', c.oid::pg_catalog.int8, 'namespace_oid', n.oid::pg_catalog.int8, 'row_type_oid', c.reltype::pg_catalog.int8,
 'relation_version', c.xmin::pg_catalog.text, 'kind', c.relkind, 'persistence', c.relpersistence,
 'partition', c.relispartition, 'row_security', c.relrowsecurity,
 'force_row_security', c.relforcerowsecurity,
 'inheritance', EXISTS(SELECT 1 FROM pg_catalog.pg_inherits i WHERE i.inhrelid=c.oid OR i.inhparent=c.oid),
 'constraints', EXISTS(SELECT 1 FROM pg_catalog.pg_constraint x WHERE x.conrelid=c.oid OR x.confrelid=c.oid),
 'indexes', EXISTS(SELECT 1 FROM pg_catalog.pg_index x WHERE x.indrelid=c.oid),
 'constraint_facts', COALESCE((SELECT pg_catalog.json_agg(f.fact ORDER BY f.oid) FROM (
 SELECT x.oid, pg_catalog.json_build_object(
 'oid', x.oid::pg_catalog.int8, 'version', x.xmin::pg_catalog.text,
 'index_oid', x.conindid::pg_catalog.int8, 'type', x.contype,
 'ordinary', x.contype IN ('p','u') AND x.conrelid=c.oid AND x.confrelid=0
 AND NOT x.condeferrable AND NOT x.condeferred AND x.convalidated AND x.conislocal
 AND x.coninhcount=0 AND x.conparentid=0,
 'catalog_row', pg_catalog.row_to_json(x)::pg_catalog.text) AS fact
 FROM pg_catalog.pg_constraint x WHERE x.conrelid=c.oid OR x.confrelid=c.oid
 ORDER BY x.oid LIMIT 257) f), '[]'::pg_catalog.json),
 'index_facts', COALESCE((SELECT pg_catalog.json_agg(f.fact ORDER BY f.oid) FROM (
 SELECT x.indexrelid AS oid, pg_catalog.json_build_object(
 'oid', x.indexrelid::pg_catalog.int8, 'name', ic.relname,
 'version', ic.xmin::pg_catalog.text || ':' || x.xmin::pg_catalog.text,
 'unique', x.indisunique, 'primary', x.indisprimary,
 'ordinary', ic.relkind='i' AND am.amname='btree' AND x.indisvalid AND x.indisready
 AND x.indislive AND x.indimmediate AND NOT x.indisexclusion
 AND x.indexprs IS NULL AND x.indpred IS NULL AND x.indnkeyatts>0
 AND NOT EXISTS(SELECT 1 FROM pg_catalog.unnest(x.indkey::pg_catalog.int2[]) k(num) WHERE k.num<=0)
 AND NOT EXISTS(SELECT 1 FROM ROWS FROM (pg_catalog.unnest(x.indkey::pg_catalog.int2[]), pg_catalog.unnest(x.indcollation::pg_catalog.oid[]))
 WITH ORDINALITY k(num, collation_oid, pos)
 LEFT JOIN pg_catalog.pg_attribute a ON a.attrelid=c.oid AND a.attnum=k.num
 WHERE a.attnum IS NULL OR (k.pos<=x.indnkeyatts AND k.collation_oid IS DISTINCT FROM a.attcollation))
 AND NOT EXISTS(SELECT 1 FROM pg_catalog.unnest(x.indclass::pg_catalog.oid[]) k(oid)
 LEFT JOIN pg_catalog.pg_opclass op ON op.oid=k.oid
 LEFT JOIN pg_catalog.pg_namespace ons ON ons.oid=op.opcnamespace
 WHERE op.oid IS NULL OR ons.nspname<>'pg_catalog' OR NOT op.opcdefault),
 'catalog_row', pg_catalog.row_to_json(x)::pg_catalog.text) AS fact
 FROM pg_catalog.pg_index x JOIN pg_catalog.pg_class ic ON ic.oid=x.indexrelid
 JOIN pg_catalog.pg_am am ON am.oid=ic.relam WHERE x.indrelid=c.oid
 ORDER BY x.indexrelid LIMIT 257) f), '[]'::pg_catalog.json),
 'triggers', EXISTS(SELECT 1 FROM pg_catalog.pg_trigger x WHERE x.tgrelid=c.oid),
 'rules', EXISTS(SELECT 1 FROM pg_catalog.pg_rewrite x WHERE x.ev_class=c.oid),
 'columns', COALESCE((SELECT pg_catalog.json_agg(a.fact ORDER BY a.num) FROM (
 SELECT a.attnum AS num, pg_catalog.json_build_object(
 'number', a.attnum, 'name', a.attname, 'type_oid', a.atttypid::pg_catalog.int8,
 'type_schema', COALESCE(tn.nspname,''), 'type_name', COALESCE(t.typname,''),
 'type_modifier', a.atttypmod, 'not_null', a.attnotnull, 'dropped', a.attisdropped,
 'default', a.atthasdef OR EXISTS(SELECT 1 FROM pg_catalog.pg_attrdef d WHERE d.adrelid=a.attrelid AND d.adnum=a.attnum),
 'default_oid', COALESCE(ad.oid::pg_catalog.int8,0), 'default_version', COALESCE(ad.xmin::pg_catalog.text,''),
 'default_expression', COALESCE(ad.adbin::pg_catalog.text,''),
 'ordinary_default', COALESCE(pg_catalog.left(ad.adbin::pg_catalog.text,
 pg_catalog.length('{CONST :consttype ' || a.atttypid::pg_catalog.text || ' '))=
 '{CONST :consttype ' || a.atttypid::pg_catalog.text || ' ',false),
 'identity', a.attidentity, 'generated', a.attgenerated, 'version', a.xmin::pg_catalog.text,
 'collation_oid', a.attcollation::pg_catalog.int8, 'collation_schema', COALESCE(cn.nspname,''),
 'collation_name', COALESCE(co.collname,''), 'collation_version', COALESCE(co.xmin::pg_catalog.text,''),
 'collation_deterministic', COALESCE(co.collisdeterministic,false)) AS fact
 FROM pg_catalog.pg_attribute a
 LEFT JOIN pg_catalog.pg_type t ON t.oid=a.atttypid
 LEFT JOIN pg_catalog.pg_attrdef ad ON ad.adrelid=a.attrelid AND ad.adnum=a.attnum
 LEFT JOIN pg_catalog.pg_namespace tn ON tn.oid=t.typnamespace
 LEFT JOIN pg_catalog.pg_collation co ON co.oid=a.attcollation
 LEFT JOIN pg_catalog.pg_namespace cn ON cn.oid=co.collnamespace
 WHERE a.attrelid=c.oid AND a.attnum>0 ORDER BY a.attnum LIMIT 4097
 ) a), '[]'::pg_catalog.json))::pg_catalog.text,1048577)
 FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace
 WHERE n.nspname=$1 AND c.relname=$2)FQEMETA";
}
