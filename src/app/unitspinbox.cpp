#include "unitspinbox.h"

#include <QHash>

#include <cmath>

namespace unitentry {

bool prefixExponent(QChar character, int *exponent)
{
    switch (character.unicode()) {
    case 'p': *exponent = -12; return true;
    case 'n': *exponent = -9; return true;
    // Plain 'u', the MICRO SIGN U+00B5 that most keyboards emit, and GREEK
    // SMALL LETTER MU U+03BC, which some fonts and locales substitute.
    case 'u':
    case 0x00B5:
    case 0x03BC: *exponent = -6; return true;
    case 'm': *exponent = -3; return true;
    case 'k': *exponent = 3; return true;
    case 'M': *exponent = 6; return true;
    default: return false;
    }
}

int exponentForUnit(const QString &unitSuffix)
{
    static const QHash<QString, int> table = {
        {QStringLiteral("ps"), -12},      {QStringLiteral("ns"), -9},
        {QStringLiteral("us"), -6},       {QStringLiteral("µs"), -6},
        {QStringLiteral("ms"), -3},       {QStringLiteral("s"), 0},
        {QStringLiteral("fA"), -15},      {QStringLiteral("pA"), -12},
        {QStringLiteral("nA"), -9},       {QStringLiteral("uA"), -6},
        {QStringLiteral("µA"), -6},  {QStringLiteral("mA"), -3},
        {QStringLiteral("A"), 0},
        {QStringLiteral("uV"), -6},       {QStringLiteral("mV"), -3},
        {QStringLiteral("V"), 0},         {QStringLiteral("kV"), 3},
        {QStringLiteral("fF"), -15},      {QStringLiteral("pF"), -12},
        {QStringLiteral("nF"), -9},       {QStringLiteral("uF"), -6},
        {QStringLiteral("F"), 0},
        {QStringLiteral("nm"), -9},       {QStringLiteral("um"), -6},
        {QStringLiteral("mm"), -3},       {QStringLiteral("m"), 0},
        {QStringLiteral("Hz"), 0},        {QStringLiteral("kHz"), 3},
        {QStringLiteral("MHz"), 6},
        {QStringLiteral("ohm"), 0},       {QStringLiteral("kohm"), 3},
        {QStringLiteral("Mohm"), 6},
    };
    return table.value(unitSuffix.trimmed(), 0);
}

bool parse(const QString &typed, const QString &unitSuffix, const QLocale &locale, double *value)
{
    QString text = typed.trimmed();
    if (text.isEmpty())
        return false;

    int typedExponent = 0;
    const bool prefixed = prefixExponent(text.at(text.size() - 1), &typedExponent);
    if (prefixed) {
        text.chop(1);
        text = text.trimmed();
        if (text.isEmpty())
            return false;
    }

    /* The operator's locale first, then C.
     *
     * A machine set to a comma-decimal locale shows "0,05" in the box, so
     * that spelling has to parse; but "0.05" is what gets pasted out of a
     * datasheet or a script on the same machine, so that has to parse too.
     */
    bool ok = false;
    double number = locale.toDouble(text, &ok);
    if (!ok)
        number = text.toDouble(&ok);
    if (!ok)
        return false;

    if (!prefixed) {
        /* No prefix typed means the number is already in the field's own
         * displayed units, and must pass through untouched. Rescaling it by
         * the field's unit exponent - as this did before the test caught it -
         * turns a plain "250" in a nanosecond field into 250 seconds.
         */
        *value = number;
        return true;
    }

    *value = number * std::pow(10.0, typedExponent - exponentForUnit(unitSuffix));
    return true;
}

} // namespace unitentry

UnitSpinBox::UnitSpinBox(QWidget *parent)
    : QDoubleSpinBox(parent)
{
}

QString UnitSpinBox::stripAffixes(const QString &text) const
{
    QString stripped = text;
    const QString boxPrefix = prefix();
    const QString boxSuffix = suffix();
    if (!boxPrefix.isEmpty() && stripped.startsWith(boxPrefix))
        stripped = stripped.mid(boxPrefix.size());
    if (!boxSuffix.isEmpty() && stripped.endsWith(boxSuffix))
        stripped.chop(boxSuffix.size());
    return stripped.trimmed();
}

double UnitSpinBox::valueFromText(const QString &text) const
{
    const QString typed = stripAffixes(text);
    if (typed.isEmpty())
        return QDoubleSpinBox::valueFromText(text);

    // Only take over when a prefix is actually present. Everything else stays
    // with the base class, whose handling of groupings and edge cases is
    // better tested than anything added here.
    int ignored = 0;
    if (!unitentry::prefixExponent(typed.at(typed.size() - 1), &ignored))
        return QDoubleSpinBox::valueFromText(text);

    double parsed = 0.0;
    if (!unitentry::parse(typed, suffix(), locale(), &parsed))
        return QDoubleSpinBox::valueFromText(text);
    return parsed;
}

QValidator::State UnitSpinBox::validate(QString &text, int &pos) const
{
    const QString typed = stripAffixes(text);
    if (!typed.isEmpty()) {
        int ignored = 0;
        if (unitentry::prefixExponent(typed.at(typed.size() - 1), &ignored)) {
            const QString number = typed.left(typed.size() - 1).trimmed();
            /* "u" on its own is halfway to "50u", and so is "-".
             *
             * These must come back Intermediate rather than Invalid, or the
             * line edit refuses the keystroke and the prefix can never be
             * typed in the first place.
             */
            if (number.isEmpty() || number == QStringLiteral("-") ||
                number == QStringLiteral("+"))
                return QValidator::Intermediate;
            double parsed = 0.0;
            if (!unitentry::parse(typed, suffix(), locale(), &parsed))
                return QValidator::Intermediate;
            return (parsed >= minimum() && parsed <= maximum()) ? QValidator::Acceptable
                                                                : QValidator::Intermediate;
        }
    }
    return QDoubleSpinBox::validate(text, pos);
}
