/* Exported data symbols are pointers to NSString objects unless their public
 * ABI specifies a scalar/structure. Notification IDs are not all strings. */
static Word constant_export(const char *name,Word storage[4]) {
    const char *value=NULL;
    if (!strcmp(name,"___NSArray0__")) { storage[0]=sequence_empty(external_class("_OBJC_CLASS_$_NSArray")); return (Word)storage; }
    if (!strcmp(name,"___NSDictionary0__")) { storage[0]=empty_map(external_class("_OBJC_CLASS_$_NSDictionary")); return (Word)storage; }
    if (!strcmp(name,"_CGAffineTransformIdentity")) { static const double identity[6]={1,0,0,1,0,0}; return (Word)identity; }
    if (!strcmp(name,"_CGSizeZero")) { memset(storage,0,16); return (Word)storage; }
    if (!strcmp(name,"_kSecRandomDefault")) { storage[0]=0; return (Word)storage; }
    if (!strcmp(name,"_kCFBooleanTrue") || !strcmp(name,"_kCFBooleanFalse")) {
        storage[0]=number_bool(external_class("_OBJC_CLASS_$_NSNumber"),"CFBoolean",!strcmp(name,"_kCFBooleanTrue"));
        return (Word)storage;
    }
    if (!strcmp(name,"_UIAccessibilityScreenChangedNotification")) { storage[0]=1000; return (Word)storage; }
    if (!strcmp(name,"_UIBackgroundTaskInvalid")) { storage[0]=UINT64_MAX; return (Word)storage; }
    if (!strcmp(name,"_UIAccessibilityTraitNone")) { storage[0]=0; return (Word)storage; }
    if (!strcmp(name,"_UIAccessibilityTraitSelected")) { storage[0]=4; return (Word)storage; }
    if (!strcmp(name,"_UIEdgeInsetsZero") || !strcmp(name,"_CGRectZero")) { memset(storage,0,32); return (Word)storage; }
    if (!strcmp(name,"_UITableViewAutomaticDimension") || !strcmp(name,"_UIViewNoIntrinsicMetric") || !strcmp(name,"_UIWindowLevelStatusBar") || !strcmp(name,"_UIWindowLevelNormal")) {
        double scalar=!strcmp(name,"_UIWindowLevelNormal") ? 0 : !strcmp(name,"_UIWindowLevelStatusBar") ? 1000 : -1;
        memcpy(storage,&scalar,sizeof(scalar)); return (Word)storage;
    }
    const struct { const char *symbol,*value; } strings[]={
        {"_CSSearchableItemActivityIdentifier","CSSearchableItemActivityIdentifier"},
        {"_PHImageErrorKey","PHImageErrorKey"},{"_UIApplicationOpenSettingsURLString","app-settings:"},
        {"_UIApplicationOpenURLOptionsAnnotationKey","UIApplicationOpenURLOptionsAnnotationKey"},
        {"_UIApplicationOpenURLOptionsSourceApplicationKey","UIApplicationOpenURLOptionsSourceApplicationKey"},
        {"_UIApplicationStatusBarFrameUserInfoKey","UIApplicationStatusBarFrameUserInfoKey"},
        {"_UIActivityTypeAssignToContact","com.apple.UIKit.activity.AssignToContact"},
        {"_UIActivityTypeCopyToPasteboard","com.apple.UIKit.activity.CopyToPasteboard"},
        {"_UIActivityTypeMail","com.apple.UIKit.activity.Mail"},
        {"_UIActivityTypeMessage","com.apple.UIKit.activity.Message"},
        {"_UIActivityTypePrint","com.apple.UIKit.activity.Print"},
        {"_UIActivityTypeSaveToCameraRoll","com.apple.UIKit.activity.SaveToCameraRoll"},
        {"_UIActivityTypeAirDrop","com.apple.UIKit.activity.AirDrop"},
        {"_UIActivityTypeAddToReadingList","com.apple.UIKit.activity.AddToReadingList"},
        {"_UIActivityTypePostToFacebook","com.apple.UIKit.activity.PostToFacebook"},
        {"_UIActivityTypePostToTwitter","com.apple.UIKit.activity.PostToTwitter"},
        {"_UIActivityTypePostToWeibo","com.apple.UIKit.activity.PostToWeibo"},
        {"_UIActivityTypePostToTencentWeibo","com.apple.UIKit.activity.PostToTencentWeibo"},
        {"_UIActivityTypePostToFlickr","com.apple.UIKit.activity.PostToFlickr"},
        {"_UIActivityTypePostToVimeo","com.apple.UIKit.activity.PostToVimeo"},
        {"_NSLocaleCountryCode","kCFLocaleCountryCode"},{"_NSLocaleCurrencyCode","kCFLocaleCurrencyCode"},
        {"_NSLocaleLanguageCode","kCFLocaleLanguageCode"},{"_NSLocaleScriptCode","kCFLocaleScriptCode"},
        {"_kSecClass","class"},{"_kSecClassGenericPassword","genp"},{"_kSecClassKey","keys"},
        {"_kSecAttrAccessible","pdmn"},{"_kSecAttrAccessGroup","agrp"},{"_kSecAttrModificationDate","mdat"},
        {"_kSecAttrAccount","acct"},{"_kSecAttrService","svce"},{"_kSecAttrGeneric","gena"},
        {"_kSecAttrIsPermanent","perm"},{"_kSecAttrApplicationTag","atag"},{"_kSecAttrKeyType","type"},
        {"_kSecAttrKeySizeInBits","bsiz"},{"_kSecAttrSynchronizable","sync"},{"_kSecAttrKeyTypeRSA","42"},
        {"_kSecMatchLimit","m_Limit"},{"_kSecMatchLimitOne","m_LimitOne"},{"_kSecMatchLimitAll","m_LimitAll"},
        {"_kSecReturnData","r_Data"},{"_kSecReturnAttributes","r_Attributes"},{"_kSecReturnRef","r_Ref"},{"_kSecValueData","v_Data"},
        {"_kSecAttrAccessibleAfterFirstUnlock","ck"},{"_kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly","cku"},
        {"_kSecAttrAccessibleWhenUnlocked","ak"},{"_kSecAttrAccessibleWhenUnlockedThisDeviceOnly","aku"},
        {"_kSecAttrAccessibleAlways","dk"},{"_kSecAttrAccessibleAlwaysThisDeviceOnly","dku"},
        {"_kSecPrivateKeyAttrs","private"},{"_kSecPublicKeyAttrs","public"},
        {"_AVAudioSessionCategoryAmbient","AVAudioSessionCategoryAmbient"},
        {"_AVAudioSessionInterruptionTypeKey","AVAudioSessionInterruptionTypeKey"},
        {"_AVAudioSessionPortBuiltInSpeaker","Speaker"},
        {"_AVAudioSessionSilenceSecondaryAudioHintTypeKey","AVAudioSessionSilenceSecondaryAudioHintTypeKey"},
        {"_NSDefaultRunLoopMode","kCFRunLoopDefaultMode"},{"_NSRunLoopCommonModes","kCFRunLoopCommonModes"},
        {"_kCFRunLoopDefaultMode","kCFRunLoopDefaultMode"},{"_kCFRunLoopCommonModes","kCFRunLoopCommonModes"},
        {"_NSLocalizedDescriptionKey","NSLocalizedDescription"},{"_NSUnderlyingErrorKey","NSUnderlyingError"},
        {"_NSLocalizedFailureReasonErrorKey","NSLocalizedFailureReason"},{"_NSLocalizedRecoverySuggestionErrorKey","NSLocalizedRecoverySuggestion"},
        {"_NSLocalizedRecoveryOptionsErrorKey","NSLocalizedRecoveryOptions"},{"_NSRecoveryAttempterErrorKey","NSRecoveryAttempter"},
        {"_NSInvalidArgumentException","NSInvalidArgumentException"},{"_NSKeyedArchiveRootObjectKey","root"},
        {"_UIPasteboardNameGeneral","com.apple.UIKit.pboard.general"},{"_UIPasteboardNameFind","com.apple.UIKit.pboard.find"},
        {"_NSFilePathErrorKey","NSFilePath"},{"_NSKeyValueChangeNewKey","new"},{"_NSKeyValueChangeOldKey","old"},
        {"_NSURLErrorFailingURLErrorKey","NSErrorFailingURLKey"},{"_NSURLErrorFailingURLStringErrorKey","NSErrorFailingURLStringKey"},
        {"_NSUserActivityTypeBrowsingWeb","NSUserActivityTypeBrowsingWeb"},{"_UILocalNotificationDefaultSoundName","UILocalNotificationDefaultSoundName"},
        {"_NSFontAttributeName","NSFont"},{"_NSForegroundColorAttributeName","NSColor"},{"_NSBackgroundColorAttributeName","NSBackgroundColor"},
        {"_UIFontDescriptorFamilyAttribute","NSFontFamilyAttribute"},{"_UIFontDescriptorSizeAttribute","NSFontSizeAttribute"},
        {"_UIFontTextStyleBody","UICTFontTextStyleBody"},{"_UIFontTextStyleCallout","UICTFontTextStyleCallout"},
        {"_UIFontTextStyleCaption1","UICTFontTextStyleCaption1"},{"_UIFontTextStyleHeadline","UICTFontTextStyleHeadline"},
        {"_UIFontTextStyleSubheadline","UICTFontTextStyleSubheadline"},{"_UIFontTextStyleTitle3","UICTFontTextStyleTitle3"},
        {"_UIContentSizeCategoryExtraSmall","UICTContentSizeCategoryXS"},{"_UIContentSizeCategorySmall","UICTContentSizeCategoryS"},
        {"_UIContentSizeCategoryMedium","UICTContentSizeCategoryM"},{"_UIContentSizeCategoryLarge","UICTContentSizeCategoryL"},
        {"_UIContentSizeCategoryExtraLarge","UICTContentSizeCategoryXL"},{"_UIContentSizeCategoryExtraExtraLarge","UICTContentSizeCategoryXXL"},
        {"_UIContentSizeCategoryExtraExtraExtraLarge","UICTContentSizeCategoryXXXL"},
        {"_UIContentSizeCategoryAccessibilityMedium","UICTContentSizeCategoryAccessibilityM"},
        {"_UIContentSizeCategoryAccessibilityLarge","UICTContentSizeCategoryAccessibilityL"},
        {"_UIContentSizeCategoryAccessibilityExtraLarge","UICTContentSizeCategoryAccessibilityXL"},
        {"_UIContentSizeCategoryAccessibilityExtraExtraLarge","UICTContentSizeCategoryAccessibilityXXL"},
        {"_UIContentSizeCategoryAccessibilityExtraExtraExtraLarge","UICTContentSizeCategoryAccessibilityXXXL"},
        {"_UIContentSizeCategoryNewValueKey","UIContentSizeCategoryNewValueKey"},
        {"_UIKeyboardAnimationCurveUserInfoKey","UIKeyboardAnimationCurveUserInfoKey"},
        {"_UIKeyboardAnimationDurationUserInfoKey","UIKeyboardAnimationDurationUserInfoKey"},
        {"_UIKeyboardFrameBeginUserInfoKey","UIKeyboardFrameBeginUserInfoKey"},{"_UIKeyboardFrameEndUserInfoKey","UIKeyboardFrameEndUserInfoKey"},
        {"_CTCallStateConnected","CTCallStateConnected"},
        {"_UIImagePickerControllerOriginalImage","UIImagePickerControllerOriginalImage"},
        {"_UIImagePickerControllerReferenceURL","UIImagePickerControllerReferenceURL"},
        {"_UIImagePickerControllerMediaType","UIImagePickerControllerMediaType"},{"_UIImagePickerControllerImageURL","UIImagePickerControllerImageURL"},{"_UIImagePickerControllerPHAsset","UIImagePickerControllerPHAsset"},
        {"_PHImageCancelledKey","PHImageCancelledKey"},{"_PHImageResultIsDegradedKey","PHImageResultIsDegradedKey"},{"_PHImageResultIsInCloudKey","PHImageResultIsInCloudKey"},{"_PHImageResultRequestIDKey","PHImageResultRequestIDKey"},
        {"_PHPhotosErrorDomain","PHPhotosErrorDomain"},{"_kUTTypeImage","public.image"},
        {"_kUTTypeJPEG","public.jpeg"},{"_kUTTypePNG","public.png"},{"_kUTTypeText","public.text"},
        {"_kUTTagClassFilenameExtension","public.filename-extension"},{"_kUTTagClassMIMEType","public.mime-type"},
        {"_NSParagraphStyleAttributeName","NSParagraphStyle"},{"_NSShadowAttributeName","NSShadow"},{"_NSLinkAttributeName","NSLink"},
        {"_NSAttachmentAttributeName","NSAttachment"},{"_NSUnderlineStyleAttributeName","NSUnderline"},
        {"_NSStrokeColorAttributeName","NSStrokeColor"},{"_NSStrokeWidthAttributeName","NSStrokeWidth"},
        {"_kCFBundleIdentifierKey","CFBundleIdentifier"},{"_kCFBundleVersionKey","CFBundleVersion"},{"_kCFHTTPVersion1_1","HTTP/1.1"},
        {"_kCFStringTransformStripCombiningMarks","NFD; [:Nonspacing Mark:] Remove; NFC"},{"_kCFStringTransformToLatin","Any-Latin"},
         
        {"_kCFProxyAutoConfigurationJavaScriptKey","kCFProxyAutoConfigurationJavaScriptKey"},
        {"_kCFProxyAutoConfigurationURLKey","kCFProxyAutoConfigurationURLKey"},
        {"_kCFProxyHostNameKey","kCFProxyHostNameKey"},{"_kCFProxyPasswordKey","kCFProxyPasswordKey"},
        {"_kCFProxyPortNumberKey","kCFProxyPortNumberKey"},{"_kCFProxyTypeKey","kCFProxyTypeKey"},
        {"_kCFProxyTypeAutoConfigurationJavaScript","kCFProxyTypeAutoConfigurationJavaScript"},
        {"_kCFProxyTypeAutoConfigurationURL","kCFProxyTypeAutoConfigurationURL"},
        {"_kCFProxyTypeHTTP","kCFProxyTypeHTTP"},{"_kCFProxyTypeHTTPS","kCFProxyTypeHTTPS"},
        {"_kCFProxyTypeSOCKS","kCFProxyTypeSOCKS"},{"_kCFProxyTypeNone","kCFProxyTypeNone"},
        {"_kCFProxyUsernameKey","kCFProxyUsernameKey"},
        {"_kCFStreamPropertySSLPeerTrust","kCFStreamPropertySSLPeerTrust"},
        {"_kCFStreamPropertySSLSettings","kCFStreamPropertySSLSettings"},
        {"_kCFStreamPropertySocketSecurityLevel","kCFStreamPropertySocketSecurityLevel"},
        {"_kCFStreamSSLValidatesCertificateChain","kCFStreamSSLValidatesCertificateChain"},
        {"_NSHTTPCookieName","Name"},{"_NSHTTPCookieValue","Value"},{"_NSHTTPCookieDomain","Domain"},
        {"_NSHTTPCookiePath","Path"},{"_NSHTTPCookieOriginURL","OriginURL"},{"_NSHTTPCookieExpires","Expires"},
        {"_NSHTTPCookieSecure","Secure"},{"_NSHTTPCookieVersion","Version"}
    };
    for (unsigned i=0;i<COUNT(strings);++i) if (!strcmp(name,strings[i].symbol)) { value=strings[i].value; break; }
    if (!value && ((strstr(name,"Notification") && strncmp(name,"_OBJC_",6)) || !strncmp(name,"_NSURL",6) || !strncmp(name,"_CTRadioAccessTechnology",24) ||
        !strncmp(name,"_NSStream",9) || !strcmp(name,"_NSInternalInconsistencyException") || !strncmp(name,"_UIApplicationLaunchOptions",27))) value=name+1;
    if (!value) return 0;
    storage[0]=copy_string(value);
    return (Word)storage;
}
static Word resolve_constant(unsigned index) {
    return functions[index] ? 0 : constant_export(symbols[index],symbol_objects[index]);
}
