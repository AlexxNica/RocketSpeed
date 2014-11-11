// AUTOGENERATED FILE - DO NOT MODIFY!
// This file generated by Djinni from rocketspeed.djinni

package org.rocketspeed;

/**
 *
 * Status returned when a subscription/unsubscription
 * message is acknowledged and confirmed by the Cloud Service.
 *
 */
public final class SubscriptionStatus {


    /*package*/ final Status mStatus;

    /*package*/ final SequenceNumber mSeqno;

    /*package*/ final boolean mSubscribed;

    public SubscriptionStatus(
            Status status,
            SequenceNumber seqno,
            boolean subscribed) {
        this.mStatus = status;
        this.mSeqno = seqno;
        this.mSubscribed = subscribed;
    }

    public Status getStatus() {
        return mStatus;
    }

    public SequenceNumber getSeqno() {
        return mSeqno;
    }

    public boolean getSubscribed() {
        return mSubscribed;
    }
}