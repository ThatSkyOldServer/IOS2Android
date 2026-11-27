package local.ios.host;

import android.icu.text.DecimalFormat;
import android.icu.text.NumberFormat;
import android.icu.util.ULocale;
import java.math.BigInteger;

final class LocalizedFormat {
    private static ULocale cachedLocale;
    private static DecimalFormat decimal, scientific;

    private static synchronized DecimalFormat formatter(boolean exponent) {
        ULocale locale = ULocale.getDefault(ULocale.Category.FORMAT);
        if (!locale.equals(cachedLocale)) {
            decimal = (DecimalFormat) NumberFormat.getNumberInstance(locale);
            scientific = (DecimalFormat) decimal.clone();
            scientific.applyPattern("0E+00");
            cachedLocale = locale;
        }
        return (DecimalFormat) (exponent ? scientific : decimal).clone();
    }

    static String number(long bits, char code, int flags, int width, int precision) {
        boolean integer = code == 'd' || code == 'i' || code == 'u';
        boolean general = code == 'g' || code == 'G';
        boolean exponent = code == 'e' || code == 'E';
        boolean left = (flags & 1) != 0, plus = (flags & 2) != 0;
        boolean space = (flags & 4) != 0, zero = (flags & 8) != 0 && !left;
        boolean alternate = (flags & 16) != 0;
        int digits = precision < 0 ? (integer ? 0 : 6) : precision;
        double real = integer ? 0 : Double.longBitsToDouble(bits);
        if (general) {
            digits = Math.max(1, digits);
            double magnitude = Math.abs(real);
            exponent = magnitude != 0 && (magnitude < 0.0001 || magnitude >= Math.pow(10, digits));
        }
        DecimalFormat format = formatter(exponent);
        if (general || exponent) {
            format.setSignificantDigitsUsed(true);
            int significant = general ? digits : digits + 1;
            format.setMaximumSignificantDigits(significant);
            format.setMinimumSignificantDigits(general && !alternate ? 1 : significant);
        } else {
            format.setMaximumFractionDigits(integer ? 0 : digits);
            format.setMinimumFractionDigits(integer ? 0 : digits);
        }
        if (!integer) format.setDecimalSeparatorAlwaysShown(alternate);
        if (plus || space) format.setPositivePrefix((plus ? "+" : " ") + format.getPositivePrefix());
        // Integer precision pads the localized field first; width then adds spaces.
        boolean integerPrecision = integer && precision >= 0;
        format.setFormatWidth(integerPrecision ? digits : width);
        format.setPadCharacter(integerPrecision || zero ? '0' : ' ');
        format.setPadPosition(integerPrecision || zero ? DecimalFormat.PAD_AFTER_PREFIX :
            left ? DecimalFormat.PAD_AFTER_SUFFIX : DecimalFormat.PAD_BEFORE_PREFIX);
        String result;
        if (!integer) result = format.format(real);
        else if (code == 'u' && bits < 0) result = format.format(new BigInteger(Long.toUnsignedString(bits)));
        else result = format.format(bits);
        if (integerPrecision && result.length() < width) {
            StringBuilder padded = new StringBuilder(width);
            if (left) padded.append(result);
            for (int i = result.length(); i < width; i++) padded.append(' ');
            if (!left) padded.append(result);
            result = padded.toString();
        }
        return result;
    }
}
