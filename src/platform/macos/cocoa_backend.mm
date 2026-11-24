#include "layout/layout_engine.hpp"
#include "platform/backend.hpp"

#import <Cocoa/Cocoa.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

static NSColor* parseHexColorObjC(const std::string& hex, NSColor* fallback) {
    if (hex.empty()) return fallback;
    std::string s = hex;
    if (s[0] == '#') s = s.substr(1);
    if (s.size() == 3 || s.size() == 4) {
        std::string exp;
        for (char c : s) { exp.push_back(c); exp.push_back(c); }
        s = exp;
    }
    if (s.size() != 6 && s.size() != 8) return fallback;
    char* end = nullptr;
    unsigned long long rgba = std::strtoull(s.c_str(), &end, 16);
    if (!end || *end != '\0') return fallback;
    if (s.size() == 6) {
        CGFloat r = ((rgba >> 16) & 0xFF) / 255.0;
        CGFloat g = ((rgba >> 8) & 0xFF) / 255.0;
        CGFloat b = (rgba & 0xFF) / 255.0;
        return [NSColor colorWithSRGBRed:r green:g blue:b alpha:1.0];
    }
    CGFloat r = ((rgba >> 24) & 0xFF) / 255.0;
    CGFloat g = ((rgba >> 16) & 0xFF) / 255.0;
    CGFloat b = ((rgba >> 8) & 0xFF) / 255.0;
    CGFloat a = (rgba & 0xFF) / 255.0;
    return [NSColor colorWithSRGBRed:r green:g blue:b alpha:a];
}

// Modern custom-styled native button with true background, border, radius,
// SF Symbol icon, hover & pressed states (replaces legacy gray macOS bezel!)
@interface TransomStyledButton : NSButton {
@public
    NSColor* normalBg;
    NSColor* hoverBg;
    NSColor* textColor;
    double fontSize;
    NSFontWeight fontWeight;
    CGFloat cornerRadius;
    CGFloat horizPad;
    CGFloat vertPad;
    BOOL isHovered;
    BOOL isPressed;
}
- (void)refreshAppearance;
@end

@implementation TransomStyledButton
- (instancetype)initWithFrame:(NSRect)frameRect {
    self = [super initWithFrame:frameRect];
    if (self) {
        [self setBordered:NO];
        [self setWantsLayer:YES];
        [self setFocusRingType:NSFocusRingTypeNone];
        [self setImagePosition:NSImageLeft];
        [self setImageHugsTitle:YES];
        normalBg = [[NSColor colorWithSRGBRed:1.0 green:1.0 blue:1.0 alpha:1.0] retain];
        hoverBg = nil;
        textColor = [[NSColor colorWithSRGBRed:0.06 green:0.09 blue:0.16 alpha:1.0] retain];
        fontSize = 13.0;
        fontWeight = NSFontWeightMedium;
        cornerRadius = 8.0;
        horizPad = 14.0;
        vertPad = 7.0;
        isHovered = NO;
        isPressed = NO;
        [self layer].borderColor = [[NSColor colorWithSRGBRed:0.89 green:0.91 blue:0.94 alpha:1.0] CGColor];
        [self layer].borderWidth = 1.0;
        [self refreshAppearance];
    }
    return self;
}

- (void)dealloc {
    [normalBg release];
    [hoverBg release];
    [textColor release];
    [super dealloc];
}

- (NSSize)intrinsicContentSize {
    NSSize s = [super intrinsicContentSize];
    return NSMakeSize(s.width + horizPad * 2.0, std::max<CGFloat>(s.height + vertPad * 2.0, 32.0));
}

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    for (NSTrackingArea* area in [self trackingAreas]) {
        [self removeTrackingArea:area];
    }
    NSTrackingAreaOptions opts = NSTrackingMouseEnteredAndExited | NSTrackingActiveAlways | NSTrackingInVisibleRect;
    NSTrackingArea* area = [[NSTrackingArea alloc] initWithRect:NSZeroRect options:opts owner:self userInfo:nil];
    [self addTrackingArea:area];
    [area release];
}

- (void)mouseEntered:(NSEvent*)event {
    [super mouseEntered:event];
    isHovered = YES;
    [[NSCursor pointingHandCursor] set];
    [self refreshAppearance];
}

- (void)mouseExited:(NSEvent*)event {
    [super mouseExited:event];
    isHovered = NO;
    isPressed = NO;
    [[NSCursor arrowCursor] set];
    [self refreshAppearance];
}

- (void)mouseDown:(NSEvent*)event {
    isPressed = YES;
    [self refreshAppearance];
    [super mouseDown:event];
    isPressed = NO;
    [self refreshAppearance];
}

- (void)refreshAppearance {
    NSColor* bg = normalBg;
    if (isPressed && bg) {
        bg = [bg blendedColorWithFraction:0.15 ofColor:[NSColor blackColor]] ?: bg;
    } else if (isHovered && bg) {
        bg = hoverBg ? hoverBg : ([bg blendedColorWithFraction:0.06 ofColor:[NSColor blackColor]] ?: bg);
    }
    [self layer].backgroundColor = bg ? [bg CGColor] : [[NSColor clearColor] CGColor];
    [self layer].cornerRadius = cornerRadius;

    NSString* title = [self title] ?: @"";
    if ([title length] == 0 && [self image]) {
        [self setImagePosition:NSImageOnly];
    } else {
        [self setImagePosition:NSImageLeft];
    }
    NSMutableParagraphStyle* ps = [[NSMutableParagraphStyle alloc] init];
    [ps setAlignment:NSTextAlignmentCenter];
    NSDictionary* attrs = @{
        NSFontAttributeName: [NSFont systemFontOfSize:(CGFloat)fontSize weight:fontWeight],
        NSForegroundColorAttributeName: textColor ?: [NSColor labelColor],
        NSParagraphStyleAttributeName: ps
    };
    NSAttributedString* attrTitle = [[NSAttributedString alloc] initWithString:title attributes:attrs];
    [self setAttributedTitle:attrTitle];
    [attrTitle release];
    [ps release];
    if ([self image] && textColor) {
        [self setContentTintColor:textColor];
    }
}
@end

// Objective-C target/delegate bridging AppKit events back to C++ callbacks
@interface TransomTarget : NSObject <NSTextFieldDelegate> {
@public
    std::function<void()> onClick;
    std::function<void(transom::Value)> onChange;
    int controlKind; // 0=Button, 1=Slider, 2=Toggle, 3=Select
}
- (void)handleAction:(id)sender;
@end

@implementation TransomTarget
- (void)handleAction:(id)sender {
    if (controlKind == 1 && onChange) {
        NSSlider* s = (NSSlider*)sender;
        onChange(transom::Value([s doubleValue]));
    } else if (controlKind == 2 && onChange) {
        NSButton* b = (NSButton*)sender;
        onChange(transom::Value([b state] == NSControlStateValueOn));
    } else if (controlKind == 3 && onChange) {
        NSPopUpButton* pop = (NSPopUpButton*)sender;
        NSString* title = [pop titleOfSelectedItem];
        onChange(transom::Value(std::string(title ? [title UTF8String] : "")));
    } else if (onClick) {
        onClick();
    }
}

- (void)controlTextDidChange:(NSNotification*)note {
    if (!onChange) return;
    NSTextField* tf = (NSTextField*)[note object];
    NSString* str = [tf stringValue];
    onChange(transom::Value(std::string(str ? [str UTF8String] : "")));
}
@end

struct CanvasStroke {
    std::string color;
    double width = 3.0;
    std::vector<NSPoint> points;
};

// CoreGraphics 2D Canvas view with optional live freehand mouse/trackpad drawing!
@interface TransomCanvasView : NSView {
@public
    std::string bgColor;
    std::string drawScript;
    bool interactive;
    std::string strokeColor;
    double strokeWidth;
    std::vector<CanvasStroke> strokes;
}
@end

@implementation TransomCanvasView
- (BOOL)isFlipped { return YES; }

- (void)mouseDown:(NSEvent*)event {
    if (!interactive) { [super mouseDown:event]; return; }
    NSPoint pt = [self convertPoint:[event locationInWindow] fromView:nil];
    CanvasStroke s;
    s.color = strokeColor.empty() ? "#3b5bdb" : strokeColor;
    s.width = strokeWidth > 0 ? strokeWidth : 3.0;
    s.points.push_back(pt);
    strokes.push_back(std::move(s));
    [self setNeedsDisplay:YES];
}

- (void)mouseDragged:(NSEvent*)event {
    if (!interactive || strokes.empty()) { [super mouseDragged:event]; return; }
    NSPoint pt = [self convertPoint:[event locationInWindow] fromView:nil];
    strokes.back().points.push_back(pt);
    [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirtyRect {
    [super drawRect:dirtyRect];
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    if (!ctx) return;

    NSColor* bg = parseHexColorObjC(bgColor, [NSColor colorWithSRGBRed:0.95 green:0.96 blue:0.98 alpha:1.0]);
    CGContextSetFillColorWithColor(ctx, [bg CGColor]);
    CGContextFillRect(ctx, [self bounds]);

    std::stringstream ss(drawScript);
    std::string cmd;
    while (std::getline(ss, cmd, ';')) {
        std::istringstream ls(cmd);
        std::string op;
        if (!(ls >> op)) continue;
        if (op == "rect") {
            double x = 0, y = 0, w = 0, h = 0;
            std::string col;
            ls >> x >> y >> w >> h >> col;
            NSColor* c = parseHexColorObjC(col, [NSColor whiteColor]);
            CGContextSetFillColorWithColor(ctx, [c CGColor]);
            CGContextFillRect(ctx, CGRectMake(x, y, w, h));
        } else if (op == "circle") {
            double cx = 0, cy = 0, r = 0;
            std::string col;
            ls >> cx >> cy >> r >> col;
            NSColor* c = parseHexColorObjC(col, [NSColor whiteColor]);
            CGContextSetFillColorWithColor(ctx, [c CGColor]);
            CGContextFillEllipseInRect(ctx, CGRectMake(cx - r, cy - r, r * 2.0, r * 2.0));
        } else if (op == "line") {
            double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
            std::string col;
            ls >> x1 >> y1 >> x2 >> y2 >> col;
            NSColor* c = parseHexColorObjC(col, [NSColor whiteColor]);
            CGContextSetStrokeColorWithColor(ctx, [c CGColor]);
            CGContextSetLineWidth(ctx, 2.0);
            CGContextMoveToPoint(ctx, x1, y1);
            CGContextAddLineToPoint(ctx, x2, y2);
            CGContextStrokePath(ctx);
        }
    }

    // Render interactive freehand strokes
    CGContextSetLineCap(ctx, kCGLineCapRound);
    CGContextSetLineJoin(ctx, kCGLineJoinRound);
    for (const auto& s : strokes) {
        if (s.points.empty()) continue;
        NSColor* c = parseHexColorObjC(s.color, [NSColor systemBlueColor]);
        CGContextSetStrokeColorWithColor(ctx, [c CGColor]);
        CGContextSetLineWidth(ctx, (CGFloat)s.width);
        CGContextMoveToPoint(ctx, s.points[0].x, s.points[0].y);
        for (size_t i = 1; i < s.points.size(); ++i) {
            CGContextAddLineToPoint(ctx, s.points[i].x, s.points[i].y);
        }
        CGContextStrokePath(ctx);
    }
}
@end

// Frame-based container: all positions come from the portable Transom layout engine.
@interface TransomLayoutView : NSView {
@public
    std::function<void()> onResize;
}
@end

@implementation TransomLayoutView
- (BOOL)isFlipped { return YES; }
- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    if (onResize) onResize();
}
@end

@interface TransomWindowDelegate : NSObject <NSWindowDelegate>
@end

@implementation TransomWindowDelegate
- (void)windowWillClose:(NSNotification*)note {
    (void)note;
    [[NSApplication sharedApplication] terminate:nil];
}
@end

@interface TransomTickTarget : NSObject {
@public
    std::function<void()> onTick;
}
- (void)tick:(NSTimer*)timer;
@end

@implementation TransomTickTarget
- (void)tick:(NSTimer*)timer {
    (void)timer;
    if (onTick) onTick();
}
@end

namespace transom {

namespace {

struct WidgetEntry {
    std::string kind;
    NSView* view = nil;
    TransomLayoutView* docView = nil; // ScrollView document view (children host)
    TransomTarget* target = nil;
    double fontSize = 14.0;
    NSFontWeight fontWeight = NSFontWeightRegular;
    CGFloat iconSize = 18.0;
    std::string iconName;
    void* nativeInst = nullptr; // plugin-provided widget instance

    // Layout (owned by transom::layout)
    layout::LayoutKind lkind = layout::LayoutKind::Leaf;
    layout::LayoutStyle style;
    bool isContainer = false;
    bool hidden = false;
    std::vector<WidgetHandle> children;
};

struct TimerEntry {
    NSTimer* timer = nil;
    TransomTickTarget* target = nil;
};

NSFontWeight parseWeight(const std::string& w) {
    if (w == "bold") return NSFontWeightBold;
    if (w == "semibold") return NSFontWeightSemibold;
    if (w == "medium") return NSFontWeightMedium;
    if (w == "light") return NSFontWeightLight;
    return NSFontWeightRegular;
}

class CocoaBackend final : public PlatformBackend {
public:
    CocoaBackend() {
        @autoreleasepool {
            NSApplication* app = [NSApplication sharedApplication];
            [app setActivationPolicy:NSApplicationActivationPolicyRegular];
            NSMenu* mainMenu = [[NSMenu alloc] init];
            NSMenuItem* appItem = [[NSMenuItem alloc] init];
            [mainMenu addItem:appItem];
            NSMenu* appMenu = [[NSMenu alloc] init];
            [appMenu addItem:[[NSMenuItem alloc] initWithTitle:@"Quit Transom"
                                                        action:@selector(terminate:)
                                                 keyEquivalent:@"q"]];
            [appItem setSubmenu:appMenu];
            [app setMainMenu:mainMenu];
        }
    }

    void setNativeWidgetHost(NativeWidgetHost* host) override { nativeHost_ = host; }

    ~CocoaBackend() override {
        @autoreleasepool {
            clearWindow();
            if (nativeHost_) {
                for (auto& [h, e] : widgets_) {
                    if (e.nativeInst) nativeHost_->destroyNativeWidget(e.kind, e.nativeInst);
                }
                widgets_.clear();
            }
            if (window_) {
                [window_ setDelegate:nil];
                [window_ close];
                [window_ release];
                window_ = nil;
            }
            if (winDelegate_) {
                [winDelegate_ release];
                winDelegate_ = nil;
            }
        }
    }

    void createWindow(const WindowConfig& cfg) override {
        @autoreleasepool {
            NSRect rect = NSMakeRect(0, 0, cfg.width, cfg.height);
            NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                               NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
            if (!window_) {
                window_ = [[NSWindow alloc] initWithContentRect:rect
                                                      styleMask:style
                                                        backing:NSBackingStoreBuffered
                                                          defer:NO];
                winDelegate_ = [[TransomWindowDelegate alloc] init];
                [window_ setDelegate:winDelegate_];
                [window_ center];
            } else {
                [window_ setContentSize:NSMakeSize(cfg.width, cfg.height)];
            }
            [window_ setTitle:[NSString stringWithUTF8String:cfg.title.c_str()]];
            setWindowBg(cfg.bg);
        }
    }

    void setWindowBg(const std::string& hex) override {
        if (!window_ || hex.empty()) return;
        @autoreleasepool {
            NSColor* bg = parseHexColorObjC(hex, [NSColor windowBackgroundColor]);
            NSColor* rgb = [bg colorUsingColorSpace:[NSColorSpace sRGBColorSpace]];
            if (rgb) {
                double lum = 0.299 * [rgb redComponent] + 0.587 * [rgb greenComponent] + 0.114 * [rgb blueComponent];
                NSAppearanceName appName = (lum > 0.5) ? NSAppearanceNameAqua : NSAppearanceNameDarkAqua;
                [window_ setAppearance:[NSAppearance appearanceNamed:appName]];
            }
            [window_ setBackgroundColor:bg];
            [[window_ contentView] setWantsLayer:YES];
            [[window_ contentView] layer].backgroundColor = [bg CGColor];
        }
    }

    void clearWindow() override {
        @autoreleasepool {
            for (auto& tm : timers_) {
                if (tm.timer) [tm.timer invalidate];
                if (tm.target) [tm.target release];
            }
            timers_.clear();

            if (rootView_) {
                [rootView_ removeFromSuperview];
                rootView_ = nil;
            }
            rootHandle_ = kInvalidWidget;
            for (auto& [id, entry] : widgets_) {
                if (entry.target) [entry.target release];
                if (entry.view) [entry.view release];
            }
            widgets_.clear();
        }
    }

    // ------------------------------------------------------------------
    // Layout is owned by the portable Transom layout engine (src/layout).
    // AppKit only provides leaf intrinsic sizes and receives final frames.
    // ------------------------------------------------------------------
    static bool applyLayoutProp(WidgetEntry& e, const std::string& name, const Value& v) {
        if (name == "width") { e.style.width = (float)v.asFloat(); return true; }
        if (name == "height") { e.style.height = (float)v.asFloat(); return true; }
        if (name == "minWidth") { e.style.minWidth = (float)v.asFloat(); return true; }
        if (name == "maxWidth") { e.style.maxWidth = (float)v.asFloat(); return true; }
        if (name == "minHeight") { e.style.minHeight = (float)v.asFloat(); return true; }
        if (name == "maxHeight") { e.style.maxHeight = (float)v.asFloat(); return true; }
        if (name == "grow") { e.style.grow = (float)v.asFloat(); return true; }
        if (name == "shrink") { e.style.shrink = (float)v.asFloat(); return true; }
        if (e.kind == "Spacer" && name == "size") {
            e.style.width = e.style.height = (float)v.asFloat();
            return true;
        }
        if (e.isContainer) {
            if (name == "gap") { e.style.gap = (float)v.asFloat(); return true; }
            if (name == "padding") { e.style.padding = (float)v.asFloat(); return true; }
            if (name == "align") { e.style.align = layout::parseAlign(v.asString(), e.style.align); return true; }
            if (name == "justify") { e.style.justify = layout::parseJustify(v.asString(), e.style.justify); return true; }
            if (name == "columns") { e.style.columns = std::max(1, (int)v.asInt()); return true; }
            if (name == "rtl") { e.style.rtl = v.asBool(); return true; }
        }
        return false;
    }

    void markDirty() {
        if (layoutScheduled_) return;
        layoutScheduled_ = true;
        dispatch_async(dispatch_get_main_queue(), ^{
            layoutScheduled_ = false;
            relayout();
        });
    }

    void measureLeaf(WidgetEntry& e, layout::LayoutStyle& st) {
        NSSize s = NSMakeSize(0, 0);
        if (e.kind == "Text") {
            NSTextField* l = (NSTextField*)e.view;
            if (st.width >= 0) {
                [l setLineBreakMode:NSLineBreakByWordWrapping];
                [[l cell] setWraps:YES];
                [l setPreferredMaxLayoutWidth:st.width];
                s = [[l cell] cellSizeForBounds:NSMakeRect(0, 0, st.width, 100000)];
            } else {
                s = [[l cell] cellSizeForBounds:NSMakeRect(0, 0, 100000, 100000)];
            }
            s.width += 2;
        } else if (e.kind == "Button") {
            s = [(NSButton*)e.view intrinsicContentSize];
        } else if (e.kind == "Icon") {
            s = NSMakeSize(e.iconSize, e.iconSize);
        } else if (e.kind == "Image") {
            s = NSMakeSize(120, 120);
        } else if (e.kind == "Input") {
            s = NSMakeSize(200, std::max<CGFloat>(24, [(NSTextField*)e.view intrinsicContentSize].height));
        } else if (e.kind == "Slider") {
            s = NSMakeSize(200, 22);
        } else if (e.kind == "Toggle") {
            s = [(NSButton*)e.view intrinsicContentSize];
        } else if (e.kind == "Select") {
            NSSize ps = [(NSPopUpButton*)e.view intrinsicContentSize];
            s = NSMakeSize(std::max<CGFloat>(160, ps.width), std::max<CGFloat>(24, ps.height));
        } else if (e.kind == "Progress") {
            s = NSMakeSize(200, 12);
        } else if (e.kind == "Canvas") {
            s = NSMakeSize(260, 160);
        } else if (e.kind == "Divider") {
            s = NSMakeSize(0, 1);
        } else if (e.kind == "Spacer") {
            s = NSMakeSize(0, 0);
        } else if (e.nativeInst) {
            s = NSMakeSize(200, 200);
        }
        st.intrinsicWidth = (float)std::ceil(s.width);
        st.intrinsicHeight = (float)std::ceil(s.height);
    }

    layout::LayoutNode buildNode(WidgetHandle h, float scale) {
        layout::LayoutNode n;
        n.id = h;
        auto it = widgets_.find(h);
        if (it == widgets_.end()) return n;
        WidgetEntry& e = it->second;
        n.kind = e.lkind;
        n.style = e.style;
        n.style.dpiScale = scale;
        if (!e.isContainer) measureLeaf(e, n.style);
        for (WidgetHandle c : e.children) {
            auto cit = widgets_.find(c);
            if (cit == widgets_.end() || cit->second.hidden) continue;
            n.children.push_back(buildNode(c, scale));
        }
        return n;
    }

    void applyFrames(const layout::LayoutNode& n, float px, float py, bool isRoot) {
        auto it = widgets_.find(n.id);
        if (it == widgets_.end()) return;
        WidgetEntry& e = it->second;
        if (e.view && !isRoot) {
            NSRect r = NSMakeRect(n.frame.x - px, n.frame.y - py, n.frame.width, n.frame.height);
            if (!NSEqualRects([e.view frame], r)) [e.view setFrame:r];
        }
        for (const auto& ch : n.children) applyFrames(ch, n.frame.x, n.frame.y, false);
        if (e.docView) {
            float maxB = 0;
            for (const auto& ch : n.children) maxB = std::max(maxB, ch.frame.bottom() - n.frame.y);
            float hgt = std::max(n.frame.height, maxB + e.style.padding);
            [e.docView setFrame:NSMakeRect(0, 0, n.frame.width, hgt)];
        }
    }

    void relayout() {
        if (inRelayout_ || rootHandle_ == kInvalidWidget || !window_) return;
        if (widgets_.find(rootHandle_) == widgets_.end()) return;
        inRelayout_ = true;
        @autoreleasepool {
            NSSize cs = [[window_ contentView] bounds].size;
            float scale = (float)[window_ backingScaleFactor];
            layout::LayoutNode tree = buildNode(rootHandle_, scale);
            layout::computeLayout(tree, (float)cs.width, (float)cs.height);
            applyFrames(tree, 0, 0, true);
        }
        inRelayout_ = false;
    }

    WidgetHandle createWidget(const std::string& kind) override {
        @autoreleasepool {
            WidgetEntry e;
            e.kind = kind;
            using layout::Align;
            using layout::LayoutKind;

            auto makeContainer = [&](LayoutKind lk, Align a, float gap, float pad) {
                TransomLayoutView* v = [[TransomLayoutView alloc] initWithFrame:NSZeroRect];
                [v setWantsLayer:YES];
                e.view = v;
                e.lkind = lk;
                e.isContainer = true;
                e.style.align = a;
                e.style.gap = gap;
                e.style.padding = pad;
                e.style.fillCross = true;
            };

            if (kind == "VStack") {
                makeContainer(LayoutKind::VStack, Align::Center, 8, 0);
            } else if (kind == "HStack") {
                makeContainer(LayoutKind::HStack, Align::Center, 8, 0);
            } else if (kind == "Grid") {
                makeContainer(LayoutKind::Grid, Align::Start, 10, 0);
            } else if (kind == "ZStack") {
                makeContainer(LayoutKind::ZStack, Align::Center, 0, 0);
            } else if (kind == "Modal") {
                makeContainer(LayoutKind::ZStack, Align::Center, 0, 0);
                [e.view layer].backgroundColor = [[NSColor colorWithCalibratedWhite:0.0 alpha:0.45] CGColor];
            } else if (kind == "Card") {
                makeContainer(LayoutKind::Card, Align::Center, 8, 16);
                [e.view layer].backgroundColor = [parseHexColorObjC("#ffffff", [NSColor whiteColor]) CGColor];
                [e.view layer].cornerRadius = 12.0;
            } else if (kind == "ScrollView") {
                NSScrollView* sv = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 360, 220)];
                [sv setHasVerticalScroller:YES];
                [sv setDrawsBackground:NO];
                TransomLayoutView* doc = [[TransomLayoutView alloc] initWithFrame:NSMakeRect(0, 0, 340, 220)];
                [sv setDocumentView:doc];
                e.view = sv;
                e.docView = doc;
                [doc release];
                e.lkind = LayoutKind::ScrollView;
                e.isContainer = true;
                e.style.align = Align::Center;
                e.style.gap = 8;
                e.style.fillCross = true;
                e.style.noShrink = true;
            } else if (kind == "Spacer") {
                NSView* sp = [[NSView alloc] initWithFrame:NSZeroRect];
                e.view = sp;
                e.lkind = LayoutKind::Spacer;
            } else if (kind == "Divider") {
                NSView* line = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 200, 1)];
                [line setWantsLayer:YES];
                [line layer].backgroundColor = [parseHexColorObjC("#e2e8f0", [NSColor separatorColor]) CGColor];
                e.view = line;
                e.style.fillCross = true;
            } else if (kind == "Icon") {
                NSImageView* iv = [[NSImageView alloc] initWithFrame:NSMakeRect(0, 0, 18, 18)];
                [iv setImageScaling:NSImageScaleProportionallyUpOrDown];
                e.view = iv;
            } else if (kind == "Text") {
                NSTextField* lbl = [[NSTextField alloc] initWithFrame:NSZeroRect];
                [lbl setBezeled:NO];
                [lbl setDrawsBackground:NO];
                [lbl setEditable:NO];
                [lbl setSelectable:NO];
                [lbl setTextColor:[NSColor colorWithSRGBRed:0.06 green:0.09 blue:0.16 alpha:1.0]];
                [lbl setAlignment:NSTextAlignmentCenter];
                [lbl setFont:[NSFont systemFontOfSize:14.0 weight:NSFontWeightRegular]];
                e.view = lbl;
            } else if (kind == "Button") {
                TransomStyledButton* btn = [[TransomStyledButton alloc] initWithFrame:NSZeroRect];
                e.target = [[TransomTarget alloc] init];
                e.target->controlKind = 0;
                [btn setTarget:e.target];
                [btn setAction:@selector(handleAction:)];
                e.view = btn;
            } else if (kind == "Input") {
                NSTextField* tf = [[NSTextField alloc] initWithFrame:NSZeroRect];
                [tf setBezelStyle:NSTextFieldRoundedBezel];
                [tf setEditable:YES];
                [tf setSelectable:YES];
                e.target = [[TransomTarget alloc] init];
                e.target->controlKind = 0;
                [tf setDelegate:e.target];
                e.view = tf;
            } else if (kind == "Slider") {
                NSSlider* sl = [[NSSlider alloc] initWithFrame:NSZeroRect];
                [sl setContinuous:YES];
                [sl setMinValue:0.0];
                [sl setMaxValue:100.0];
                e.target = [[TransomTarget alloc] init];
                e.target->controlKind = 1;
                [sl setTarget:e.target];
                [sl setAction:@selector(handleAction:)];
                e.view = sl;
            } else if (kind == "Toggle") {
                NSButton* cb = [[NSButton alloc] initWithFrame:NSZeroRect];
                [cb setButtonType:NSButtonTypeSwitch];
                e.target = [[TransomTarget alloc] init];
                e.target->controlKind = 2;
                [cb setTarget:e.target];
                [cb setAction:@selector(handleAction:)];
                e.view = cb;
            } else if (kind == "Select") {
                NSPopUpButton* pop = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
                e.target = [[TransomTarget alloc] init];
                e.target->controlKind = 3;
                [pop setTarget:e.target];
                [pop setAction:@selector(handleAction:)];
                e.view = pop;
            } else if (kind == "Image") {
                NSImageView* iv = [[NSImageView alloc] initWithFrame:NSMakeRect(0, 0, 120, 120)];
                [iv setImageScaling:NSImageScaleProportionallyUpOrDown];
                e.view = iv;
            } else if (kind == "Progress") {
                NSProgressIndicator* pi = [[NSProgressIndicator alloc] initWithFrame:NSZeroRect];
                [pi setIndeterminate:NO];
                [pi setStyle:NSProgressIndicatorStyleBar];
                [pi setMinValue:0.0];
                [pi setMaxValue:100.0];
                e.view = pi;
            } else if (kind == "Canvas") {
                TransomCanvasView* cv = [[TransomCanvasView alloc] initWithFrame:NSMakeRect(0, 0, 260, 160)];
                [cv setWantsLayer:YES];
                [cv layer].cornerRadius = 10.0;
                cv->interactive = false;
                cv->strokeColor = "#3b5bdb";
                cv->strokeWidth = 3.0;
                e.view = cv;
            } else if (nativeHost_ && nativeHost_->hasNativeWidget(kind)) {
                void* inst = nativeHost_->createNativeWidget(kind);
                if (!inst) return kInvalidWidget;
                e.nativeInst = inst;
                e.view = (NSView*)nativeHost_->nativeWidgetHandle(kind, inst);
            }

            if (e.view) [e.view setTranslatesAutoresizingMaskIntoConstraints:YES];

            WidgetHandle h = nextHandle_++;
            widgets_[h] = e;
            markDirty();
            return h;
        }
    }

    void setProp(WidgetHandle h, const std::string& name, const Value& value) override {
        auto it = widgets_.find(h);
        if (it == widgets_.end()) return;
        WidgetEntry& e = it->second;

        @autoreleasepool {
            struct DirtyOnExit {
                CocoaBackend* b;
                ~DirtyOnExit() { b->markDirty(); }
            } dirtyOnExit{this};

            // Universal appearance props
            if (name == "opacity" && e.view) {
                [e.view setAlphaValue:(CGFloat)value.asFloat()];
                return;
            }
            if (name == "borderColor" && e.view) {
                [e.view setWantsLayer:YES];
                NSColor* c = parseHexColorObjC(value.toString(), [NSColor clearColor]);
                [e.view layer].borderColor = [c CGColor];
                return;
            }
            if (name == "borderWidth" && e.view) {
                [e.view setWantsLayer:YES];
                [e.view layer].borderWidth = (CGFloat)value.asFloat();
                return;
            }
            if (name == "shadow" && e.view) {
                CGFloat elev = (CGFloat)value.asFloat();
                if (elev > 0) {
                    NSShadow* sh = [[NSShadow alloc] init];
                    [sh setShadowColor:[NSColor colorWithSRGBRed:0.06 green:0.09 blue:0.16 alpha:0.10]];
                    [sh setShadowOffset:NSMakeSize(0, elev * 0.4)];
                    [sh setShadowBlurRadius:elev * 1.6];
                    [e.view setShadow:sh];
                    [sh release];
                } else {
                    [e.view setShadow:nil];
                }
                return;
            }
            if (applyLayoutProp(e, name, value)) return;

            if (e.isContainer) {
                if (e.kind == "Modal" && name == "open") {
                    e.hidden = !value.asBool();
                    [e.view setHidden:e.hidden];
                } else if (name == "bg" && e.view) {
                    [e.view setWantsLayer:YES];
                    NSColor* c = parseHexColorObjC(value.asString(), [NSColor clearColor]);
                    [e.view layer].backgroundColor = [c CGColor];
                } else if (name == "radius" && e.view) {
                    [e.view setWantsLayer:YES];
                    [e.view layer].cornerRadius = (CGFloat)value.asFloat();
                }
            } else if (e.kind == "Divider") {
                if (name == "color") {
                    [e.view layer].backgroundColor =
                        [parseHexColorObjC(value.asString(), [NSColor separatorColor]) CGColor];
                }
            } else if (e.kind == "Icon") {
                NSImageView* iv = (NSImageView*)e.view;
                if (name == "name") {
                    e.iconName = value.asString();
                    NSString* sym = [NSString stringWithUTF8String:e.iconName.c_str()];
                    NSImage* img = [NSImage imageWithSystemSymbolName:sym accessibilityDescription:nil];
                    if (img) [iv setImage:img];
                } else if (name == "size") {
                    e.iconSize = (CGFloat)value.asFloat();
                } else if (name == "color") {
                    [iv setContentTintColor:parseHexColorObjC(value.asString(), [NSColor labelColor])];
                }
            } else if (e.kind == "Text") {
                NSTextField* lbl = (NSTextField*)e.view;
                if (name == "text") {
                    [lbl setStringValue:[NSString stringWithUTF8String:value.toString().c_str()]];
                } else if (name == "size") {
                    e.fontSize = value.asFloat();
                    [lbl setFont:[NSFont systemFontOfSize:(CGFloat)e.fontSize weight:e.fontWeight]];
                } else if (name == "weight") {
                    e.fontWeight = parseWeight(value.asString());
                    [lbl setFont:[NSFont systemFontOfSize:(CGFloat)e.fontSize weight:e.fontWeight]];
                } else if (name == "color") {
                    [lbl setTextColor:parseHexColorObjC(value.asString(), [NSColor labelColor])];
                } else if (name == "align") {
                    const std::string& a = value.asString();
                    if (a == "left" || a == "leading") [lbl setAlignment:NSTextAlignmentLeft];
                    else if (a == "right" || a == "trailing") [lbl setAlignment:NSTextAlignmentRight];
                    else [lbl setAlignment:NSTextAlignmentCenter];
                }
            } else if (e.kind == "Button") {
                TransomStyledButton* btn = (TransomStyledButton*)e.view;
                if (name == "label") {
                    [btn setTitle:[NSString stringWithUTF8String:value.toString().c_str()]];
                    [btn refreshAppearance];
                } else if (name == "icon") {
                    NSString* sym = [NSString stringWithUTF8String:value.asString().c_str()];
                    NSImage* img = [NSImage imageWithSystemSymbolName:sym accessibilityDescription:nil];
                    if (img) [btn setImage:img];
                    [btn refreshAppearance];
                } else if (name == "bg") {
                    [btn->normalBg release];
                    btn->normalBg = [parseHexColorObjC(value.asString(), [NSColor whiteColor]) retain];
                    [btn refreshAppearance];
                } else if (name == "color") {
                    [btn->textColor release];
                    btn->textColor = [parseHexColorObjC(value.asString(), [NSColor labelColor]) retain];
                    [btn refreshAppearance];
                } else if (name == "radius") {
                    btn->cornerRadius = (CGFloat)value.asFloat();
                    [btn refreshAppearance];
                } else if (name == "size") {
                    btn->fontSize = value.asFloat();
                    [btn refreshAppearance];
                } else if (name == "weight") {
                    btn->fontWeight = parseWeight(value.asString());
                    [btn refreshAppearance];
                } else if (name == "padding") {
                    btn->horizPad = (CGFloat)value.asFloat();
                    btn->vertPad = (CGFloat)value.asFloat() * 0.5;
                    [btn invalidateIntrinsicContentSize];
                } else if (name == "style") {
                    const std::string& st = value.asString();
                    if (st == "primary") {
                        [btn->normalBg release];
                        btn->normalBg = [parseHexColorObjC("#3b5bdb", [NSColor systemBlueColor]) retain];
                        [btn->textColor release];
                        btn->textColor = [[NSColor whiteColor] retain];
                        [btn layer].borderWidth = 0.0;
                        [btn setKeyEquivalent:@"\r"];
                        [btn refreshAppearance];
                    } else if (st == "soft") {
                        [btn->normalBg release];
                        btn->normalBg = [parseHexColorObjC("#eff6ff", [NSColor whiteColor]) retain];
                        [btn->textColor release];
                        btn->textColor = [parseHexColorObjC("#2563eb", [NSColor systemBlueColor]) retain];
                        [btn layer].borderColor = [parseHexColorObjC("#bfdbfe", [NSColor clearColor]) CGColor];
                        [btn layer].borderWidth = 1.0;
                        [btn refreshAppearance];
                    } else if (st == "ghost") {
                        [btn->normalBg release];
                        btn->normalBg = [[NSColor clearColor] retain];
                        [btn layer].borderWidth = 0.0;
                        [btn refreshAppearance];
                    }
                }
            } else if (e.kind == "Input") {
                NSTextField* tf = (NSTextField*)e.view;
                if (name == "placeholder") {
                    [tf setPlaceholderString:[NSString stringWithUTF8String:value.toString().c_str()]];
                } else if (name == "bind") {
                    NSString* current = [tf stringValue];
                    std::string next = value.toString();
                    if (!current || next != [current UTF8String]) {
                        [tf setStringValue:[NSString stringWithUTF8String:next.c_str()]];
                    }
                }
            } else if (e.kind == "Slider") {
                NSSlider* sl = (NSSlider*)e.view;
                if (name == "min") [sl setMinValue:value.asFloat()];
                else if (name == "max") [sl setMaxValue:value.asFloat()];
                else if (name == "bind") {
                    if (std::fabs([sl doubleValue] - value.asFloat()) > 1e-6) {
                        [sl setDoubleValue:value.asFloat()];
                    }
                }
            } else if (e.kind == "Toggle") {
                NSButton* cb = (NSButton*)e.view;
                if (name == "label") [cb setTitle:[NSString stringWithUTF8String:value.toString().c_str()]];
                else if (name == "bind") [cb setState:value.asBool() ? NSControlStateValueOn : NSControlStateValueOff];
            } else if (e.kind == "Select") {
                NSPopUpButton* pop = (NSPopUpButton*)e.view;
                if (name == "options" && value.isList()) {
                    [pop removeAllItems];
                    if (auto list = value.asList()) {
                        for (const auto& item : *list) {
                            [pop addItemWithTitle:[NSString stringWithUTF8String:item.toString().c_str()]];
                        }
                    }
                } else if (name == "bind") {
                    [pop selectItemWithTitle:[NSString stringWithUTF8String:value.toString().c_str()]];
                }
            } else if (e.kind == "Image") {
                NSImageView* iv = (NSImageView*)e.view;
                if (name == "src") {
                    NSImage* img = [[NSImage alloc] initWithContentsOfFile:
                        [NSString stringWithUTF8String:value.toString().c_str()]];
                    [iv setImage:img];
                    [img release];
                }
            } else if (e.kind == "Progress") {
                NSProgressIndicator* pi = (NSProgressIndicator*)e.view;
                if (name == "value") [pi setDoubleValue:value.asFloat()];
                else if (name == "max") [pi setMaxValue:value.asFloat()];
            } else if (e.kind == "Canvas") {
                TransomCanvasView* cv = (TransomCanvasView*)e.view;
                if (name == "bg") { cv->bgColor = value.asString(); [cv setNeedsDisplay:YES]; }
                else if (name == "draw") {
                    cv->drawScript = value.asString();
                    if (cv->drawScript.empty()) cv->strokes.clear();
                    [cv setNeedsDisplay:YES];
                } else if (name == "interactive") {
                    cv->interactive = value.asBool();
                } else if (name == "strokeColor") {
                    cv->strokeColor = value.asString();
                } else if (name == "strokeWidth") {
                    cv->strokeWidth = value.asFloat();
                }
            } else if (e.nativeInst && nativeHost_) {
                nativeHost_->setNativeWidgetProp(e.kind, e.nativeInst, name, value);
            }
        }
    }

    void addChild(WidgetHandle parent, WidgetHandle child) override {
        auto pit = widgets_.find(parent);
        auto cit = widgets_.find(child);
        if (pit == widgets_.end() || cit == widgets_.end() || !pit->second.isContainer) return;
        @autoreleasepool {
            NSView* host = pit->second.docView ? (NSView*)pit->second.docView : pit->second.view;
            if (cit->second.view) [host addSubview:cit->second.view];
            pit->second.children.push_back(child);
            markDirty();
        }
    }

    void clearChildren(WidgetHandle parent) override {
        auto pit = widgets_.find(parent);
        if (pit == widgets_.end() || !pit->second.isContainer) return;
        @autoreleasepool {
            NSView* host = pit->second.docView ? (NSView*)pit->second.docView : pit->second.view;
            NSArray<NSView*>* subs = [[host subviews] copy];
            for (NSView* v in subs) [v removeFromSuperview];
            [subs release];
            pit->second.children.clear();
            markDirty();
        }
    }

    void setRoot(WidgetHandle root) override {
        auto it = widgets_.find(root);
        if (it == widgets_.end() || !window_) return;
        @autoreleasepool {
            NSView* content = [window_ contentView];
            rootView_ = it->second.view;
            rootHandle_ = root;
            [rootView_ setTranslatesAutoresizingMaskIntoConstraints:YES];
            [rootView_ setFrame:[content bounds]];
            [rootView_ setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
            [content addSubview:rootView_];
            if ([rootView_ isKindOfClass:[TransomLayoutView class]]) {
                ((TransomLayoutView*)rootView_)->onResize = [this]() { relayout(); };
            }
            relayout();
        }
    }

    void setOnClick(WidgetHandle h, std::function<void()> cb) override {
        auto it = widgets_.find(h);
        if (it == widgets_.end()) return;
        if (it->second.nativeInst && nativeHost_) {
            nativeHost_->setNativeWidgetEventSink(it->second.kind, it->second.nativeInst,
                [cb](const std::string&, const Value&) { if (cb) cb(); });
            return;
        }
        if (!it->second.target) return;
        it->second.target->onClick = std::move(cb);
    }

    void setOnChange(WidgetHandle h, std::function<void(Value)> cb) override {
        auto it = widgets_.find(h);
        if (it == widgets_.end() || !it->second.target) return;
        it->second.target->onChange = std::move(cb);
    }

    void addTimer(int intervalMs, std::function<void()> cb) override {
        @autoreleasepool {
            TransomTickTarget* target = [[TransomTickTarget alloc] init];
            target->onTick = std::move(cb);
            NSTimer* timer = [NSTimer scheduledTimerWithTimeInterval:(intervalMs / 1000.0)
                                                              target:target
                                                            selector:@selector(tick:)
                                                            userInfo:nil
                                                             repeats:YES];
            timers_.push_back({timer, target});
        }
    }

    void dispatchToMain(std::function<void()> cb) override {
        auto fn = std::make_shared<std::function<void()>>(std::move(cb));
        dispatch_async(dispatch_get_main_queue(), ^{
            if (*fn) (*fn)();
        });
    }

    void clipboardCopy(const std::string& text) override {
        @autoreleasepool {
            NSPasteboard* pb = [NSPasteboard generalPasteboard];
            [pb clearContents];
            [pb setString:[NSString stringWithUTF8String:text.c_str()]
                  forType:NSPasteboardTypeString];
        }
    }

    std::string clipboardPaste() override {
        @autoreleasepool {
            NSPasteboard* pb = [NSPasteboard generalPasteboard];
            NSString* s = [pb stringForType:NSPasteboardTypeString];
            return s ? std::string([s UTF8String]) : "";
        }
    }

    void openUrl(const std::string& url) override {
        @autoreleasepool {
            NSURL* u = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
            if (u) [[NSWorkspace sharedWorkspace] openURL:u];
        }
    }

    std::string dialogOpenFile() override {
        @autoreleasepool {
            NSOpenPanel* panel = [NSOpenPanel openPanel];
            [panel setCanChooseFiles:YES];
            [panel setCanChooseDirectories:NO];
            [panel setAllowsMultipleSelection:NO];
            if ([panel runModal] == NSModalResponseOK) {
                NSURL* url = [[panel URLs] firstObject];
                if (url) return std::string([[url path] UTF8String]);
            }
            return "";
        }
    }

    std::string dialogSaveFile() override {
        @autoreleasepool {
            NSSavePanel* panel = [NSSavePanel savePanel];
            if ([panel runModal] == NSModalResponseOK) {
                NSURL* url = [panel URL];
                if (url) return std::string([[url path] UTF8String]);
            }
            return "";
        }
    }

    void httpGetAsync(const std::string& url,
                      std::function<void(std::string)> onDone) override {
        httpRequestAsync("GET", url, "", "", std::move(onDone));
    }

    void httpRequestAsync(const std::string& method,
                          const std::string& url,
                          const std::string& headers,
                          const std::string& body,
                          std::function<void(std::string)> onDone) override {
        @autoreleasepool {
            NSURL* nsUrl = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
            if (!nsUrl) {
                if (onDone) onDone("");
                return;
            }
            NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:nsUrl];
            [req setHTTPMethod:[NSString stringWithUTF8String:method.empty() ? "GET" : method.c_str()]];

            // Parse "Key: Value\nKey2: Value2" headers
            std::istringstream hs(headers);
            std::string hline;
            while (std::getline(hs, hline)) {
                size_t col = hline.find(':');
                if (col == std::string::npos) continue;
                std::string k = hline.substr(0, col);
                std::string v = hline.substr(col + 1);
                while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
                while (!v.empty() && (v.back() == '\r' || v.back() == '\n')) v.pop_back();
                [req setValue:[NSString stringWithUTF8String:v.c_str()]
                    forHTTPHeaderField:[NSString stringWithUTF8String:k.c_str()]];
            }

            if (!body.empty()) {
                NSData* data = [NSData dataWithBytes:body.data() length:body.size()];
                [req setHTTPBody:data];
            }

            auto cb = std::make_shared<std::function<void(std::string)>>(std::move(onDone));
            NSURLSessionDataTask* task =
                [[NSURLSession sharedSession]
                    dataTaskWithRequest:req
                      completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
                          (void)response;
                          std::string out;
                          if (!error && data && [data length] > 0) {
                              out.assign(reinterpret_cast<const char*>([data bytes]), [data length]);
                          }
                          if (*cb) (*cb)(std::move(out));
                      }];
            [task resume];
        }
    }

    void runEventLoop(std::function<void()> onTick) override {
        @autoreleasepool {
            relayout();
            if (window_) {
                [window_ makeKeyAndOrderFront:nil];
                [[NSApplication sharedApplication] activateIgnoringOtherApps:YES];
            }
            TransomTickTarget* tickTarget = nil;
            NSTimer* timer = nil;
            if (onTick) {
                tickTarget = [[TransomTickTarget alloc] init];
                tickTarget->onTick = std::move(onTick);
                timer = [NSTimer scheduledTimerWithTimeInterval:0.25
                                                         target:tickTarget
                                                       selector:@selector(tick:)
                                                       userInfo:nil
                                                        repeats:YES];
            }
            [[NSApplication sharedApplication] run];
            if (timer) [timer invalidate];
            if (tickTarget) [tickTarget release];
        }
    }

private:
    NSWindow* window_ = nil;
    NSView* rootView_ = nil;
    TransomWindowDelegate* winDelegate_ = nil;
    WidgetHandle nextHandle_ = 1;
    NativeWidgetHost* nativeHost_ = nullptr;
    WidgetHandle rootHandle_ = kInvalidWidget;
    bool layoutScheduled_ = false;
    bool inRelayout_ = false;
    std::unordered_map<WidgetHandle, WidgetEntry> widgets_;
    std::vector<TimerEntry> timers_;
};

} // namespace

std::unique_ptr<PlatformBackend> createNativeBackend() {
    return std::make_unique<CocoaBackend>();
}

} // namespace transom
