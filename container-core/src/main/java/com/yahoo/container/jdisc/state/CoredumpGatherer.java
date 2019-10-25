// Copyright 2019 Oath Inc. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.
package com.yahoo.container.jdisc.state;

import com.yahoo.vespa.defaults.Defaults;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.IOException;
import java.io.UncheckedIOException;
import java.nio.file.Files;
import java.nio.file.NoSuchFileException;
import java.nio.file.Path;
import java.time.Instant;
import java.util.Set;

/**
 * @author olaa
 */
public class CoredumpGatherer {

    private static final Path COREDUMP_PATH = Path.of(Defaults.getDefaults().underVespaHome("var/crash/processing"));

    public static JSONObject gatherCoredumpMetrics(FileWrapper fileWrapper) {
        int coredumps = getNumberOfCoredumps(fileWrapper);
        JSONObject packet = new JSONObject();

        try {
            packet.put("status_code", coredumps == 0 ? 0 : 1);
            packet.put("status_msg", coredumps == 0 ? "OK" : String.format("Found %d coredump(s)", coredumps));
            packet.put("timestamp", Instant.now().getEpochSecond());
            packet.put("application", "system-coredumps-processing");

        } catch (JSONException e) {}
        return packet;
    }

    private static int getNumberOfCoredumps(FileWrapper fileWrapper) {
        try {
            return (int) fileWrapper.walkTree(COREDUMP_PATH)
                    .filter(fileWrapper::isRegularFile)
                    .count();
        } catch (NoSuchFileException e) {
            return 0;
        } catch (IOException e) {
            throw new UncheckedIOException(e);
        }
    }
}
