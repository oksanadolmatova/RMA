
CREATE TABLE test456 (col1 INT) ;

INSERT INTO test456 VALUES (20080101) ;
INSERT INTO test456 VALUES (20080227) ;
INSERT INTO test456 VALUES (20080215) ;
INSERT INTO test456 VALUES (NULL) ;
INSERT INTO test456 VALUES (NULL) ;

-- Query 1

SELECT col1, SUBSTRING(col1,1,4), SUBSTRING(col1 ,5,2), SUBSTRING(col1 ,7,2)
FROM   test456  ;


-- Query 2

SELECT CASE
         WHEN col1 IS NOT NULL
           THEN CAST(SUBSTRING(col1 ,1,4)||'-'||SUBSTRING(col1
,5,2)||'-'||SUBSTRING(col1 ,7,2) AS DATE)
       END
FROM   test456 ;

-- Query 3

SELECT CAST(SUBSTRING(col1 ,1,4)||'-'||SUBSTRING(col1
,5,2)||'-'||SUBSTRING(col1 ,7,2) AS DATE)
FROM   test456
WHERE  col1 IS NOT NULL ;

DROP TABLE test456 ;