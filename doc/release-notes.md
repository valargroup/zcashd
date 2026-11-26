(note: this is a temporary file, to be added-to by anybody, and moved to
release-notes at release time)

Notable changes
===============

Lower wallet transaction fees
----------------------------

The wallet now charges 1,000 zatoshis per logical action, down from 5,000.
It charges for at least two actions, so the minimum conventional fee falls
from 10,000 to 2,000 zatoshis. The node also uses the new rate to count
unpaid actions when deciding which transactions to admit to its mempool.

The wallet still limits explicit fees to four times the conventional fee.
A caller that supplies a fixed fee based on the old rate may need to lower it.
