--
-- Cypher Query Language - Property Index
--
DROP ROLE IF EXISTS regressrole;
CREATE ROLE regressrole SUPERUSER;
SET ROLE regressrole;

--
-- CREATE GRAPH
--

SHOW graph_path;
CREATE GRAPH propidx;
SHOW graph_path;


CREATE VLABEL piv1;

CREATE PROPERTY INDEX ON piv1 (name);
CREATE PROPERTY INDEX ON piv1 (name.first, name.last);
CREATE PROPERTY INDEX ON piv1 ((name.first + name.last));
CREATE PROPERTY INDEX ON piv1 (age);
CREATE PROPERTY INDEX ON piv1 ((body.weight / body.height));

\d propidx.piv1
\dGi piv1*

-- Check property name & access method type
CREATE VLABEL piv2;

CREATE PROPERTY INDEX ON piv2 (name);
CREATE PROPERTY INDEX ON piv2 USING btree (name.first);
CREATE PROPERTY INDEX ON piv2 USING hash (name.first);
CREATE PROPERTY INDEX ON piv2 USING brin (name.first);
CREATE PROPERTY INDEX ON piv2 USING gin (name);
CREATE PROPERTY INDEX ON piv2 USING gist (name);

--CREATE PROPERTY INDEX ON piv2 USING gin ((self_intro::tsvector));
--CREATE PROPERTY INDEX ON piv2 USING gist ((hobby::tsvector));

\d propidx.piv2
\dGv+ piv2
\dGi piv2*

-- Concurrently build & if not exist
CREATE VLABEL piv3;

CREATE PROPERTY INDEX CONCURRENTLY ON piv3 (name.first);
CREATE PROPERTY INDEX IF NOT EXISTS piv3_first_idx ON piv3 (name.first);

-- Collation & Sort & NULL order
--CREATE PROPERTY INDEX ON piv3 (name.first COLLATE "C" ASC NULLS FIRST);

-- Tablespace
CREATE PROPERTY INDEX ON piv3 (name) TABLESPACE pg_default;

-- Storage parameter & partial index
CREATE PROPERTY INDEX ON piv3 (name.first) WITH (fillfactor = 80);
CREATE PROPERTY INDEX ON piv3 (name.first) WHERE (name IS NOT NULL);

\d propidx.piv3
\dGv+ piv3
\dGi piv3*

-- Unique property index
CREATE VLABEL piv4;

CREATE UNIQUE PROPERTY INDEX ON piv4 (id);
CREATE (:piv4 {id: 100});
CREATE (:piv4 {id: 100});

\d propidx.piv4
\dGv+ piv4
\dGi piv4*

-- Multi-column unique property index
CREATE VLABEL piv5;

CREATE UNIQUE PROPERTY INDEX ON piv5 (name.first, name.last);
CREATE (:piv5 {name: {first: 'agens'}});
CREATE (:piv5 {name: {first: 'agens'}});
CREATE (:piv5 {name: {first: 'agens', last: 'graph'}});
CREATE (:piv5 {name: {first: 'agens', last: 'graph'}});

\d propidx.piv5
\dGv+ piv5
\dGi piv5*

-- DROP PROPERTY INDEX
CREATE VLABEL piv6;

CREATE PROPERTY INDEX piv6_idx ON piv6 (name);

DROP PROPERTY INDEX piv6_idx;
DROP PROPERTY INDEX IF EXISTS piv6_idx;
DROP PROPERTY INDEX piv6_pkey;

DROP VLABEL piv6;

CREATE ELABEL pie1;
CREATE PROPERTY INDEX pie1_idx ON pie1 (reltype);

DROP PROPERTY INDEX pie1_idx;
DROP PROPERTY INDEX IF EXISTS pie1_idx;
DROP PROPERTY INDEX pie1_id_idx;
DROP PROPERTY INDEX pie1_start_idx;
DROP PROPERTY INDEX pie1_end_idx;

DROP ELABEL pie1;

CREATE VLABEL piv7;

CREATE PROPERTY INDEX piv7_multi_col ON piv7 (name.first, name.middle, name.last);
\dGv+ piv7

EXPLAIN MATCH (n:piv7) WHERE n.name.first = 'Today' AND n.name.middle = 'Is' AND n.name.last = 'Tuesday' RETURN n;
EXPLAIN MATCH (n:piv7) WHERE n.name.first = to_jsonb('Today'::text) AND n.name.middle = to_jsonb('Is'::text) AND n.name.last = to_jsonb('Tuesday'::text) RETURN n;

\dGi piv7*
DROP PROPERTY INDEX piv7_multi_col;

CREATE PROPERTY INDEX piv7_multi_expr ON piv7 ((name.first + name.last), age);
\dGv+ piv7
\dGi piv7*
DROP PROPERTY INDEX piv7_multi_expr;

DROP VLABEL piv7;

-- wrong case
CREATE VLABEL piv8;

CREATE PROPERTY INDEX piv8_index_key1 ON piv8 (key1);
CREATE PROPERTY INDEX piv8_index_key1 ON piv8 (key1);

CREATE PROPERTY INDEX ON nonexsist_name (key1);

DROP VLABEL piv8;

CREATE VLABEL piv9;

CREATE PROPERTY INDEX piv9_property_index_key1 ON piv9 (key1);
DROP INDEX propidx.piv9_property_index_key1;

CREATE INDEX piv9_index_key1 ON propidx.piv9 (properties);
DROP PROPERTY INDEX piv9_index_key1;

DROP VLABEL piv9;

CREATE VLABEL piv10;

CREATE PROPERTY INDEX piv10_index_name ON piv10 USING BTREE((name::text) text_pattern_ops);

CREATE (:piv10 {name: 'Alex Jones'})
CREATE (:piv10 {name: 'Logan Hayes'})
CREATE (:piv10 {name: 'Sidney Holland'})
CREATE (:piv10 {name: 'Kit Johnson'})
CREATE (:piv10 {name: 'Terry Ball'})
CREATE (:piv10 {name: 'Kris Hughes'})
CREATE (:piv10 {name: 'Rowan Mclaughlin'})
CREATE (:piv10 {name: 'Jesse Rivers'})
CREATE (:piv10 {name: 'Aiden Love'})
CREATE (:piv10 {name: 'Vic Trevino'})
CREATE (:piv10 {name: 'Vic Mullen'});

EXPLAIN MATCH (n:piv10) WHERE n.name::text ~~ 'V%'::text RETURN n.name;
MATCH (n:piv10) WHERE n.name::text ~~ 'V%'::text RETURN n.name;

DROP PROPERTY INDEX piv10_index_name;

DROP VLABEL piv10 CASCADE;

CREATE VLABEL piv11;
CREATE VLABEL piv12;
CREATE VLABEL piv13;
CREATE PROPERTY INDEX piv12_index_incusers ON piv12 ((any(excusers in metadata['conditions']['users']['excuserscount'] where excusers > 5)));
\dGi piv12*
EXPLAIN MATCH (a)
OPTIONAL MATCH (a)-[]->(d1:piv12)
where
    any(excusers in d1.'metadata'.'conditions'.'users'.'excuserscount' where excusers > 5)
with distinct(a) as r,
collect((d1 IS NOT NULL)) as passed ,
    collect({'SecResID' : d1.'id', 'ResType' : d1.'type'}) as secResDetailsd1 return r, any(p in passed where p) as passed, secResDetailsd1 as secresdetails;


EXPLAIN MATCH (d1:piv12) where d1.id = 1
return d1;

-- teardown
DROP GRAPH propidx CASCADE;
RESET ROLE;
