/* syntax version 1 */
/* postgres can not */
/* custom check: len(yt_res_yson[0][b'Write'][0][b'Data']) < 6 */

select * from (select * from plato.Input) tablesample bernoulli(80) limit 5;
