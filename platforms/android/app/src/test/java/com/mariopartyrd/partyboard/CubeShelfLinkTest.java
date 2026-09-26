package com.mariopartyrd.partyboard;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

import java.io.File;
import java.nio.file.Files;

// Same rule as CubeShelf's PhoneLink.IsSavePath: memory cards only, never outside the folder.
public class CubeShelfLinkTest {
    @Test
    public void onlyMemoryCardsTravel() {
        assertTrue(CubeShelfLink.isSavePath("MemoryCardA.USA.raw"));
        assertTrue(CubeShelfLink.isSavePath("EUR/Card A/01-GMPP-MARIPA4BOX0.gci"));
        assertFalse(CubeShelfLink.isSavePath("config.json"));
        assertFalse(CubeShelfLink.isSavePath("../MemoryCardA.USA.raw"));
        assertFalse(CubeShelfLink.isSavePath("USA/Card A/../../x.gci"));
        assertFalse(CubeShelfLink.isSavePath("online-diagnostics/Card A/x.gci"));
    }

    @Test
    public void receivedSavesReplaceWithABackup() throws Exception {
        File files = Files.createTempDirectory("pb").toFile();
        Files.write(new File(files, "MemoryCardA.EUR.raw").toPath(), new byte[] { 1 });
        File incoming = new File(files, CubeShelfLink.INCOMING);
        incoming.mkdirs();
        Files.write(new File(incoming, "MemoryCardA.EUR.raw").toPath(), new byte[] { 2 });
        Files.write(new File(incoming, "config.json").toPath(), new byte[] { 3 }); // ignored

        assertEquals(1, CubeShelfLink.applyIncoming(files));
        assertEquals(2, Files.readAllBytes(new File(files, "MemoryCardA.EUR.raw").toPath())[0]);
        assertFalse(new File(files, "config.json").exists());
        assertFalse(incoming.exists());
        File[] backups = new File(files, CubeShelfLink.BACKUPS).listFiles();
        assertEquals(1, backups.length);
        assertEquals(1, Files.readAllBytes(new File(backups[0], "MemoryCardA.EUR.raw").toPath())[0]);
        CubeShelfLink.deleteTree(files);
    }
}
