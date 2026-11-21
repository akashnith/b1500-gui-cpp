#pragma once

#include <QDoubleSpinBox>
#include <QLocale>
#include <QString>

/*! Entry of numbers carrying SI prefixes.
 *
 *  Entering a compliance of fifty microamps into a box that shows twelve
 *  decimal places means typing `0.000050000000`, and miscounting the zeros is
 *  easy and expensive. This accepts `50u` instead. Likewise `100n` for a
 *  width, `1m` for a millisecond, `2k` for a kilohm.
 *
 *  Prefixes are case-sensitive, as SI requires: `m` is milli, `M` is mega.
 *  Recognised: p, n, u (or the micro sign), m, k, M.
 *
 *  The parsing lives here as free functions rather than inside the widget so
 *  it can be tested without a QApplication. It decides what current a real
 *  instrument is allowed to pass, so a wrong exponent is a factor of a
 *  million applied to a device; that deserves a test more than it deserves
 *  tidy encapsulation.
 */
namespace unitentry {

/*! Power of ten of a unit suffix: "ns" gives -9, "A" gives 0.
 *
 *  Matched against an explicit table. "First character looks like a prefix"
 *  would read "cm2" as centi-something, and a silent factor of a million in a
 *  measurement program is not an acceptable failure mode, so an unrecognised
 *  unit is simply unprefixed.
 */
int exponentForUnit(const QString &unitSuffix);

/*! Parse \a typed, which may end in an SI prefix, into \a unitSuffix's units.
 *
 *  The prefix is interpreted against the BASE unit, not against whatever
 *  prefix the field itself displays: in a field showing "ns", `1u` is one
 *  microsecond, which is 1000 of that field's own units - not 1e-9 of a
 *  nanosecond. Returns false if \a typed is not a number with an optional
 *  prefix, leaving \a value untouched.
 */
bool parse(const QString &typed, const QString &unitSuffix, const QLocale &locale, double *value);

//! True if \a character is an SI prefix this accepts, writing its exponent.
bool prefixExponent(QChar character, int *exponent);

} // namespace unitentry

/*! A QDoubleSpinBox that accepts the entry format above.
 *
 *  Only entry is affected. The value, range, decimals and displayed text are
 *  ordinary QDoubleSpinBox behaviour, so nothing downstream has to know.
 */
class UnitSpinBox : public QDoubleSpinBox
{
public:
    explicit UnitSpinBox(QWidget *parent = nullptr);

    double valueFromText(const QString &text) const override;
    QValidator::State validate(QString &text, int &pos) const override;

private:
    //! Strip the box's prefix and suffix, leaving what the operator typed.
    QString stripAffixes(const QString &text) const;
};
