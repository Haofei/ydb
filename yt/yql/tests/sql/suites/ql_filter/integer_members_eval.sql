pragma yt.UseQLFilter;

select a, b
from plato.Input
where a - b + 5 > 0;