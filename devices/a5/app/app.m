/*
 * Cascadia: a web view and the bridge to cascadia-helper.  The interface is
 * www/index.html; it calls back here by loading cascadia:status, cascadia:boot,
 * cascadia:update, cascadia:rollback, cascadia:latest and cascadia:copy.
 *
 * Built against stubs (the SDK has no armv7 slices), so no UIKit headers: the
 * messages this file sends are declared below.
 */
#import <objc/NSObject.h>
#import <objc/runtime.h>
#include <dispatch/dispatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HELPER "/usr/libexec/cascadia-helper"
#define FETCH "/usr/libexec/cascadia-fetch"

typedef struct { float x, y, w, h; } Rect;

extern int UIApplicationMain(int, char **, id, id);

@interface NSObject (CascadiaCalls)
+ (id)mainScreen;
+ (id)currentDevice;
+ (id)mainBundle;
+ (id)generalPasteboard;
+ (id)stringWithUTF8String:(const char *)s;
+ (id)fileURLWithPath:(id)p;
+ (id)requestWithURL:(id)u;
+ (id)colorWithRed:(float)r green:(float)g blue:(float)b alpha:(float)a;
- (Rect)bounds;
- (id)initWithFrame:(Rect)r;
- (void)setDelegate:(id)d;
- (void)setAutoresizingMask:(unsigned long)m;
- (void)setBackgroundColor:(id)c;
- (void)setOpaque:(BOOL)o;
- (id)scrollView;
- (void)setBounces:(BOOL)b;
- (void)setView:(id)v;
- (void)setRootViewController:(id)v;
- (void)makeKeyAndVisible;
- (void)setBatteryMonitoringEnabled:(BOOL)b;
- (float)batteryLevel;
- (id)pathForResource:(id)n ofType:(id)t inDirectory:(id)d;
- (void)loadRequest:(id)r;
- (id)URL;
- (id)scheme;
- (id)resourceSpecifier;
- (const char *)UTF8String;
- (id)stringByEvaluatingJavaScriptFromString:(id)s;
- (void)setString:(id)s;
@end

static id S(const char *s) { return [objc_getClass("NSString") stringWithUTF8String:s]; }
static id C(const char *n) { return (id)objc_getClass(n); }

@interface CascadiaApp : NSObject {
	id window, web, vc;
	int booting;
}
@end

@implementation CascadiaApp

- (void)js:(const char *)code
{
	char *copy = strdup(code);

	dispatch_async(dispatch_get_main_queue(), ^{
		[web stringByEvaluatingJavaScriptFromString:S(copy)];
		free(copy);
	});
}

- (void)runStatus
{
	dispatch_async(dispatch_get_global_queue(0, 0), ^{
		static char out[4096];
		char *js;
		size_t n = 0;
		float level = [[C("UIDevice") currentDevice] batteryLevel];
		FILE *p = popen(HELPER " status", "r");

		if (p) {
			n = fread(out, 1, sizeof(out) - 1, p);
			pclose(p);
		}
		out[n] = 0;
		if (asprintf(&js, "cascadiaStatus(%s,%.3f)", n ? out : "null",
			     level) < 0)
			return;
		[self js:js];
		free(js);
	});
}

- (void)runLatest
{
	dispatch_async(dispatch_get_global_queue(0, 0), ^{
		char out[256], *js;
		size_t n = 0;
		FILE *p = popen(FETCH, "r");

		if (p) {
			n = fread(out, 1, sizeof(out) - 1, p);
			pclose(p);
		}
		out[n] = 0;
		out[strcspn(out, "\r\n")] = 0;
		if (asprintf(&js, "cascadiaLatest(%s)", out[0] ? out : "null") < 0)
			return;
		[self js:js];
		free(js);
	});
}

/* boot, update, rollback: the helper's "kk text" lines to the page as they
   come, then end(rc) */
- (void)runHelper:(const char *)cmd end:(const char *)end
{
	if (booting)
		return;
	booting = 1;
	dispatch_async(dispatch_get_global_queue(0, 0), ^{
		char line[256], *js, *q, *pc;
		int rc = -1;
		FILE *p;

		if (asprintf(&pc, HELPER " %s", cmd) < 0) {
			booting = 0;
			return;
		}
		p = popen(pc, "r");
		free(pc);

		if (p) {
			while (fgets(line, sizeof(line), p)) {
				line[strcspn(line, "\r\n")] = 0;
				for (q = line; *q; q++)
					if (*q == '"' || *q == '\\' || *q == '<')
						*q = ' ';
				if (line[0] && line[1] && line[2] == ' ' &&
				    asprintf(&js, "cascadiaLine(\"%.2s\",\"%s\")",
					     line, line + 3) >= 0) {
					[self js:js];
					free(js);
				}
			}
			rc = pclose(p);
		}
		booting = 0;
		if (asprintf(&js, "%s(%d)", end, rc) >= 0) {
			[self js:js];
			free(js);
		}
	});
}

- (BOOL)application:(id)a didFinishLaunchingWithOptions:(id)o
{
	Rect r = [[C("UIScreen") mainScreen] bounds];
	id bg = [C("UIColor") colorWithRed:0.114f green:0.196f blue:0.247f alpha:1.0f];
	id path, url;

	window = [[C("UIWindow") alloc] initWithFrame:r];
	web = [[C("UIWebView") alloc] initWithFrame:r];
	[web setDelegate:self];
	[web setAutoresizingMask:18];	/* flexible width and height */
	[web setOpaque:NO];
	[web setBackgroundColor:bg];
	[[web scrollView] setBounces:NO];
	vc = [[C("UIViewController") alloc] init];
	[vc setView:web];
	[window setRootViewController:vc];
	[window makeKeyAndVisible];
	[[C("UIDevice") currentDevice] setBatteryMonitoringEnabled:YES];

	path = [[C("NSBundle") mainBundle] pathForResource:S("index")
						    ofType:S("html")
					       inDirectory:S("www")];
	url = [C("NSURL") fileURLWithPath:path];
	[web loadRequest:[C("NSURLRequest") requestWithURL:url]];

	return YES;
}

- (BOOL)webView:(id)w shouldStartLoadWithRequest:(id)req navigationType:(long)t
{
	id url = [req URL];
	const char *scheme = [[url scheme] UTF8String];
	const char *cmd;

	if (!scheme || strcmp(scheme, "cascadia"))
		return YES;
	cmd = [[url resourceSpecifier] UTF8String];
	if (!cmd)
		return NO;
	if (!strcmp(cmd, "status"))
		[self runStatus];
	else if (!strcmp(cmd, "boot"))
		[self runHelper:"boot" end:"cascadiaBootEnd"];
	else if (!strcmp(cmd, "update"))
		[self runHelper:"update" end:"cascadiaRunEnd"];
	else if (!strcmp(cmd, "rollback"))
		[self runHelper:"rollback" end:"cascadiaRunEnd"];
	else if (!strcmp(cmd, "latest"))
		[self runLatest];
	else if (!strcmp(cmd, "copy"))
		[[C("UIPasteboard") generalPasteboard] setString:
			[web stringByEvaluatingJavaScriptFromString:S("logText()")]];

	return NO;
}

- (void)webViewDidFinishLoad:(id)w
{
	[self runStatus];
}

- (void)applicationDidBecomeActive:(id)a
{
	if (!booting)
		[self runStatus];
}

@end

int main(int argc, char **argv)
{
	return UIApplicationMain(argc, argv, nil, (id)S("CascadiaApp"));
}
