/* Compat shims for building the rtw88 backport against 5.10 */
#ifndef RTW88_COMPAT_ANDR36OID_H
#define RTW88_COMPAT_ANDR36OID_H
#include <linux/version.h>
#include <linux/skbuff.h>
#include <net/mac80211.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 10, 0)
static inline void rtw_compat_purge_tx_queue(struct ieee80211_hw *hw,
					     struct sk_buff_head *skbs)
{
	struct sk_buff *skb;

	while ((skb = __skb_dequeue(skbs)))
		ieee80211_free_txskb(hw, skb);
}
#define ieee80211_purge_tx_queue rtw_compat_purge_tx_queue
#endif
#endif
