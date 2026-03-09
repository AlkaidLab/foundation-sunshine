/**
 * @file src/platform/macos/av_video.m
 * @brief Definitions for video capture on macOS.
 */
#import "av_video.h"

@implementation AVVideo

// XXX: Currently, this function only returns the screen IDs as names,
// which is not very helpful to the user. The API to retrieve names
// was deprecated with 10.9+.
// However, there is a solution with little external code that can be used:
// https://stackoverflow.com/questions/20025868/cgdisplayioserviceport-is-deprecated-in-os-x-10-9-how-to-replace
+ (NSArray<NSDictionary *> *)displayNames {
  CGDirectDisplayID displays[kMaxDisplays];
  uint32_t count;
  if (CGGetActiveDisplayList(kMaxDisplays, displays, &count) != kCGErrorSuccess) {
    return [NSArray array];
  }

  NSMutableArray *result = [NSMutableArray array];

  for (uint32_t i = 0; i < count; i++) {
    // 检查显示器是否在线
    if (!CGDisplayIsOnline(displays[i])) {
      NSLog(@"Display %u is not online, skipping", displays[i]);
      continue;
    }

    // 检查显示器是否有有效的模式
    CGDisplayModeRef mode = CGDisplayCopyDisplayMode(displays[i]);
    if (!mode) {
      NSLog(@"Failed to get display mode for display %u, skipping", displays[i]);
      continue;
    }

    size_t width = CGDisplayModeGetPixelWidth(mode);
    size_t height = CGDisplayModeGetPixelHeight(mode);
    CFRelease(mode);

    if (width == 0 || height == 0) {
      NSLog(@"Invalid resolution %zux%zu for display %u, skipping", width, height, displays[i]);
      continue;
    }

    NSString *displayName = [self getDisplayName:displays[i]];

    // macOS 26+ 兼容：确保 displayName 不为 nil
    if (!displayName) {
      displayName = [NSString stringWithFormat:@"Display %u", displays[i]];
    }

    [result addObject:@{
      @"id": [NSNumber numberWithUnsignedInt:displays[i]],
      @"name": [NSString stringWithFormat:@"%d", displays[i]],
      @"displayName": displayName,  // 保证不为 nil
    }];
  }

  return [NSArray arrayWithArray:result];
}

+ (NSString *)getDisplayName:(CGDirectDisplayID)displayID {
  NSArray *screens = [NSScreen screens];
  for (NSScreen *screen in screens) {
    if ([screen.deviceDescription[@"NSScreenNumber"] isEqual:[NSNumber numberWithUnsignedInt:displayID]]) {
      return screen.localizedName;
    }
  }
  return [NSString stringWithFormat:@"Display %u", displayID];
}

- (id)initWithDisplay:(CGDirectDisplayID)displayID frameRate:(int)frameRate {
  fprintf(stderr, "[INIT] initWithDisplay called for display %u @ %d fps\n", displayID, frameRate);
  fflush(stderr);
  self = [super init];
  fprintf(stderr, "[INIT] After [super init], self = %p\n", self);
  fflush(stderr);

  fprintf(stderr, "[PERMISSION CHECK] Starting permission check...\n");
  fflush(stderr);

  // 检查屏幕录制权限（macOS 10.15+）
  // CGPreflightScreenCaptureAccess() 会触发权限对话框（如果尚未授权）
  // 但只有在作为 .app 启动时才会显示对话框
  BOOL hasPermission = CGPreflightScreenCaptureAccess();
  fprintf(stderr, "[PERMISSION CHECK] CGPreflightScreenCaptureAccess returned: %d\n", hasPermission);
  fflush(stderr);

  if (!hasPermission) {
    fprintf(stderr, "⚠️  屏幕录制权限未授予！\n");
    fprintf(stderr, "请在 系统设置 → 隐私与安全性 → 屏幕录制 中授予 Sunshine 权限\n");
    fprintf(stderr, "然后重启 Sunshine\n");
    // 不要立即返回 nil，让 AVCaptureSession 尝试初始化
    // 这样可以触发系统权限对话框
  }
  else {
    fprintf(stderr, "✅ 屏幕录制权限已授予\n");
  }

  // 检查显示器是否在线
  if (!CGDisplayIsOnline(displayID)) {
    NSLog(@"Display %u is not online", displayID);
    return nil;
  }

  CGDisplayModeRef mode = CGDisplayCopyDisplayMode(displayID);
  if (!mode) {
    NSLog(@"Failed to get display mode for display %u", displayID);
    return nil;
  }

  self.displayID = displayID;
  self.pixelFormat = kCVPixelFormatType_32BGRA;

  // Get display dimensions
  int displayWidth = (int) CGDisplayModeGetPixelWidth(mode);
  int displayHeight = (int) CGDisplayModeGetPixelHeight(mode);

  // YUV 4:2:0 formats (NV12/P010) require even dimensions because
  // the chroma plane is half the size of the luma plane in both dimensions.
  // Align to even values to prevent "Picture size invalid" errors.
  self.frameWidth = (displayWidth + 1) & ~1;
  self.frameHeight = (displayHeight + 1) & ~1;

  // Log if alignment was needed
  if (self.frameWidth != displayWidth || self.frameHeight != displayHeight) {
    NSLog(@"[VideoToolbox] Aligned display dimensions from %dx%d to %dx%d for YUV420 compatibility",
          displayWidth, displayHeight, self.frameWidth, self.frameHeight);
  }

  CFRelease(mode);

  // 验证分辨率有效性
  if (self.frameWidth <= 0 || self.frameHeight <= 0) {
    NSLog(@"Invalid display resolution: %dx%d for display %u",
          self.frameWidth, self.frameHeight, displayID);
    return nil;
  }

  NSLog(@"Initializing display %u with resolution %dx%d @ %d fps",
        displayID, self.frameWidth, self.frameHeight, frameRate);

  NSLog(@"[DEBUG] After init: self.frameWidth=%d, self.frameHeight=%d",
        self.frameWidth, self.frameHeight);

  self.minFrameDuration = CMTimeMake(1, frameRate);
  self.session = [[AVCaptureSession alloc] init];
  self.videoOutputs = [[NSMapTable alloc] init];
  self.captureCallbacks = [[NSMapTable alloc] init];
  self.captureSignals = [[NSMapTable alloc] init];

  AVCaptureScreenInput *screenInput = [[AVCaptureScreenInput alloc] initWithDisplayID:self.displayID];
  [screenInput setMinFrameDuration:self.minFrameDuration];
  [screenInput setCapturesCursor:YES];   // 默认显示服务端光标（经典鼠标模式）
  [screenInput setCapturesMouseClicks:YES];
  [screenInput setRemovesDuplicateFrames:NO];  // 即使内容不变也持续交付帧，保证固定帧率
  self.screenInput = screenInput;

  if ([self.session canAddInput:screenInput]) {
    [self.session addInput:screenInput];
  }
  else {
    NSLog(@"Failed to add screen input for display %u", displayID);
    [screenInput release];
    return nil;
  }

  [self.session startRunning];

  return self;
}

- (void)dealloc {
  [self.videoOutputs release];
  [self.captureCallbacks release];
  [self.captureSignals release];
  [self.session stopRunning];
  [super dealloc];
}

- (void)setCursorVisible:(BOOL)visible {
  @synchronized(self) {
    if (!self.screenInput) {
      return;
    }

    if (self.screenInput.capturesCursor == visible) {
      return;
    }

    const BOOL hasActiveCaptureOutputs = self.session && [self.videoOutputs count] > 0;

    if (hasActiveCaptureOutputs) {
      [self.session beginConfiguration];
    }

    [self.screenInput setCapturesCursor:visible];

    if (hasActiveCaptureOutputs) {
      [self.session commitConfiguration];
    }
  }
}

- (void)setFrameWidth:(int)frameWidth frameHeight:(int)frameHeight {
  NSLog(@"[DEBUG] setFrameWidth called with: %dx%d", frameWidth, frameHeight);

  // YUV 4:2:0 formats (NV12/P010) require even dimensions because
  // the chroma plane is half the size of the luma plane in both dimensions.
  // Align to even values to prevent "Picture size invalid" errors.
  self.frameWidth = (frameWidth + 1) & ~1;
  self.frameHeight = (frameHeight + 1) & ~1;

  // Log if alignment was needed
  if (self.frameWidth != frameWidth || self.frameHeight != frameHeight) {
    NSLog(@"[VideoToolbox] Aligned frame dimensions from %dx%d to %dx%d for YUV420 compatibility",
          frameWidth, frameHeight, self.frameWidth, self.frameHeight);
  }

  NSLog(@"[DEBUG] After setFrameWidth: self.frameWidth=%d, self.frameHeight=%d",
        self.frameWidth, self.frameHeight);
}

- (dispatch_semaphore_t)capture:(FrameCallbackBlock)frameCallback {
  @synchronized(self) {
    AVCaptureVideoDataOutput *videoOutput = [[AVCaptureVideoDataOutput alloc] init];

    [videoOutput setVideoSettings:@{
      (NSString *) kCVPixelBufferPixelFormatTypeKey: [NSNumber numberWithUnsignedInt:self.pixelFormat],
      (NSString *) kCVPixelBufferWidthKey: [NSNumber numberWithInt:self.frameWidth],
      (NSString *) kCVPixelBufferHeightKey: [NSNumber numberWithInt:self.frameHeight],
      (NSString *) AVVideoScalingModeKey: AVVideoScalingModeResizeAspect,
    }];

    dispatch_queue_attr_t qos = dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL,
      QOS_CLASS_USER_INITIATED,
      DISPATCH_QUEUE_PRIORITY_HIGH);
    dispatch_queue_t recordingQueue = dispatch_queue_create("videoCaptureQueue", qos);
    [videoOutput setSampleBufferDelegate:self queue:recordingQueue];

    [self.session stopRunning];

    if ([self.session canAddOutput:videoOutput]) {
      [self.session addOutput:videoOutput];
    }
    else {
      [videoOutput release];
      return nil;
    }

    AVCaptureConnection *videoConnection = [videoOutput connectionWithMediaType:AVMediaTypeVideo];
    dispatch_semaphore_t signal = dispatch_semaphore_create(0);

    [self.videoOutputs setObject:videoOutput forKey:videoConnection];
    [self.captureCallbacks setObject:frameCallback forKey:videoConnection];
    [self.captureSignals setObject:signal forKey:videoConnection];

    [self.session startRunning];

    return signal;
  }
}

- (void)captureOutput:(AVCaptureOutput *)captureOutput
  didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
         fromConnection:(AVCaptureConnection *)connection {
  FrameCallbackBlock callback = [self.captureCallbacks objectForKey:connection];

  if (callback != nil) {
    if (!callback(sampleBuffer)) {
      @synchronized(self) {
        [self.session stopRunning];
        [self.captureCallbacks removeObjectForKey:connection];
        [self.session removeOutput:[self.videoOutputs objectForKey:connection]];
        [self.videoOutputs removeObjectForKey:connection];
        dispatch_semaphore_signal([self.captureSignals objectForKey:connection]);
        [self.captureSignals removeObjectForKey:connection];
        [self.session startRunning];
      }
    }
  }
}

@end
