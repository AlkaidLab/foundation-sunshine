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
    [result addObject:@{
      @"id": [NSNumber numberWithUnsignedInt:displays[i]],
      @"name": [NSString stringWithFormat:@"%d", displays[i]],
      @"displayName": [self getDisplayName:displays[i]],
    }];
  }

  return [NSArray arrayWithArray:result];
}

+ (NSString *)getDisplayName:(CGDirectDisplayID)displayID {
  for (NSScreen *screen in [NSScreen screens]) {
    if ([screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue] == displayID) {
      return screen.localizedName;
    }
  }
  // Never nil: the caller puts this in a dictionary literal, and nil there throws.
  return [NSString stringWithFormat:@"Display %u", displayID];
}

- (id)initWithDisplay:(CGDirectDisplayID)displayID frameRate:(int)frameRate {
  self = [super init];

  CGDisplayModeRef mode = CGDisplayCopyDisplayMode(displayID);

  self.displayID = displayID;
  self.pixelFormat = kCVPixelFormatType_32BGRA;
  self.frameWidth = (int) CGDisplayModeGetPixelWidth(mode);
  self.frameHeight = (int) CGDisplayModeGetPixelHeight(mode);
  self.minFrameDuration = CMTimeMake(1, frameRate);
  self.session = [[AVCaptureSession alloc] init];
  self.videoOutputs = [[NSMapTable alloc] init];
  self.captureCallbacks = [[NSMapTable alloc] init];
  self.captureSignals = [[NSMapTable alloc] init];

  self.runtimeErrorObserver = [[NSNotificationCenter defaultCenter]
    addObserverForName:AVCaptureSessionRuntimeErrorNotification
    object:self.session
    queue:nil
    usingBlock:^(NSNotification *note) {
      NSError *error = note.userInfo[AVCaptureSessionErrorKey];
      NSLog(@"[AVVideo] AVCaptureSession RUNTIME ERROR: %@", error);
    }];

  CFRelease(mode);

  AVCaptureScreenInput *screenInput = [[AVCaptureScreenInput alloc] initWithDisplayID:self.displayID];
  [screenInput setMinFrameDuration:self.minFrameDuration];

  if ([self.session canAddInput:screenInput]) {
    [self.session addInput:screenInput];
  }
  else {
    [screenInput release];
    [self release];
    return nil;
  }

  return self;
}

- (void)dealloc {
  if (self.runtimeErrorObserver) {
    [[NSNotificationCenter defaultCenter] removeObserver:self.runtimeErrorObserver];
    self.runtimeErrorObserver = nil;
  }
  [self.session stopRunning];
  [self.session release];
  for (AVCaptureOutput *out in [self.videoOutputs objectEnumerator]) {
    if ([out isKindOfClass:[AVCaptureVideoDataOutput class]]) {
      [(AVCaptureVideoDataOutput *)out setSampleBufferDelegate:nil queue:NULL];
    }
  }
  [self.videoOutputs release];
  [self.captureCallbacks release];
  [self.captureSignals release];
  [super dealloc];
}

- (void)setFrameWidth:(int)frameWidth frameHeight:(int)frameHeight {
  self.frameWidth = frameWidth;
  self.frameHeight = frameHeight;
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

    [self.session beginConfiguration];

    if ([self.session canAddOutput:videoOutput]) {
      [self.session addOutput:videoOutput];
    }
    else {
      [self.session commitConfiguration];
      [videoOutput release];
      return nil;
    }

    [self.session commitConfiguration];

    AVCaptureConnection *videoConnection = [videoOutput connectionWithMediaType:AVMediaTypeVideo];
    dispatch_semaphore_t signal = dispatch_semaphore_create(0);

    [self.videoOutputs setObject:videoOutput forKey:videoConnection];
    id copiedBlock = [frameCallback copy];
    [self.captureCallbacks setObject:copiedBlock forKey:videoConnection];
    [copiedBlock release];
    [self.captureSignals setObject:signal forKey:videoConnection];

    if (!self.session.isRunning) {
      [self.session startRunning];
    }

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
        [self.session beginConfiguration];
        AVCaptureOutput *outToRemove = [self.videoOutputs objectForKey:connection];
        if (outToRemove) {
          if ([outToRemove isKindOfClass:[AVCaptureVideoDataOutput class]]) {
            [(AVCaptureVideoDataOutput *)outToRemove setSampleBufferDelegate:nil queue:NULL];
          }
          [self.session removeOutput:outToRemove];
        }
        [self.session commitConfiguration];

        if (self.videoOutputs.count <= 1 && self.session.isRunning) {
          [self.session stopRunning];
        }

        [self.captureCallbacks removeObjectForKey:connection];
        [self.videoOutputs removeObjectForKey:connection];
        dispatch_semaphore_signal([self.captureSignals objectForKey:connection]);
        [self.captureSignals removeObjectForKey:connection];
      }
    }
  }
}

@end
