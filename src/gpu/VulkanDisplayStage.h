diff --git a/src/gpu/VulkanDisplayStage.h b/src/gpu/VulkanDisplayStage.h
index 926f6c8..0000000 100644
--- a/src/gpu/VulkanDisplayStage.h
+++ b/src/gpu/VulkanDisplayStage.h
@@
-    VulkanContext ctx_;
+    VulkanContext ctx_;
+    // Optional GPU demosaic stage; created when the display initializes the
+    // VulkanContext. If non-null, the camera will hand dmabuf fds to it for
+    // zero-copy demosaic.
+    class VulkanDemosaicStage* demosaicStage_ = nullptr;
@@
     bool initialized_ = false;
 };
