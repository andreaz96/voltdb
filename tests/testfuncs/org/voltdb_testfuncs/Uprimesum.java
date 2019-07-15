package org.voltdb_testfuncs;

import java.io.Serializable;

public class Uprimesum implements Serializable {
    private int intermediateResult = 0;

    public void start() {
        intermediateResult = 0;
    }

    public void assemble (int value) {
        if (value > 1) {
            boolean prime = true;
            for (int i = 2; i < value; ++i) {
                if (value % i == 0) {
                    prime = false;
                    break;
                }
            }
            if (prime)  intermediateResult += value;   
        }     
    }

    public void combine (Uprimesum other) {
        intermediateResult += other.intermediateResult;
    }

    public int end () {
        return intermediateResult;
    }
}