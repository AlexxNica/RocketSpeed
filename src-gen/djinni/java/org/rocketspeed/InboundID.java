// AUTOGENERATED FILE - DO NOT MODIFY!
// This file generated by Djinni from rocketspeed.djinni

package org.rocketspeed;

public final class InboundID {


    /*package*/ final byte[] serialised;

    public InboundID(
            byte[] serialised) {
        this.serialised = serialised;
    }

    public byte[] getSerialised() {
        return serialised;
    }

    @Override
    public int hashCode() {
        return java.util.Arrays.hashCode(serialised);
    }

    @Override
    public boolean equals(Object obj) {
        if (!(obj instanceof InboundID)) {
            return false;
        }
        InboundID other = (InboundID) obj;
        return java.util.Arrays.equals(serialised, other.serialised);
    }
}
