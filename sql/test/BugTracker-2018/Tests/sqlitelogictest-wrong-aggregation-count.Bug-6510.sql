CREATE TABLE tab1(col0 INTEGER, col1 INTEGER, col2 INTEGER);
INSERT INTO tab1 VALUES(51,14,96);
INSERT INTO tab1 VALUES(85,5,59);
INSERT INTO tab1 VALUES(91,47,68);
SELECT ALL CAST ( COUNT ( DISTINCT - 34 ) * - COUNT ( * ) AS BIGINT) AS col2 FROM tab1 WHERE + col1 IS NULL;
DROP TABLE tab1;