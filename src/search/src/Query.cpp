// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/search/Query.h"

#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QHash>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <limits>
#include <vector>

namespace eu::search {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("eu::search::Query", text);
}

const qint64 noLimitLow = std::numeric_limits<qint64>::min();
const qint64 noLimitHigh = std::numeric_limits<qint64>::max();

/// Familias de archivos para el filtro `type:`.
enum class TypeCategory {
    Image,
    Video,
    Audio,
    Document,
    Text,
    Archive,
    Code,
    Folder,
    File,   ///< Cualquier cosa que no sea carpeta.
};

} // namespace

/// Un nodo del árbol de la consulta.
struct Query::Node
{
    enum class Kind {
        And,
        Or,
        Not,
        Substring,   ///< El nombre contiene `text`.
        Wildcard,    ///< El nombre entero encaja con el patrón `text` (* y ?).
        Extension,   ///< El nombre termina en alguna de `list` (".pdf").
        Size,        ///< Tamaño entre `low` y `high`, ambos incluidos.
        Modified,    ///< Fecha en [low, high).
        Created,
        Type,
        Hidden,      ///< isHidden == flag.
        Regex,
        Content,     ///< El texto del archivo contiene `text`.
        Unavailable, ///< Filtro aún no implementado: nunca se cumple.
    };

    Kind kind = Kind::And;
    std::vector<std::shared_ptr<const Node>> children;
    QString text;
    QStringList list;
    qint64 low = noLimitLow;
    qint64 high = noLimitHigh;
    bool flag = false;
    Qt::CaseSensitivity caseSensitivity = Qt::CaseInsensitive;
    QRegularExpression regex;
    TypeCategory category = TypeCategory::File;
};

namespace {

using NodePtr = std::shared_ptr<const Query::Node>;
using Kind = Query::Node::Kind;

// ---------------------------------------------------------------------------
// Utilidades de texto
// ---------------------------------------------------------------------------

/// true si el texto ya está con seguridad en forma compuesta (NFC) sin tener
/// que normalizarlo. Por debajo de U+0300 no hay acentos sueltos ni letras
/// con otra forma equivalente: ahí caen el ASCII y las letras acentuadas del
/// español y de casi todos los idiomas europeos. Lo demás se normaliza.
bool isComposedForSure(QStringView text)
{
    for (const QChar character : text) {
        if (character.unicode() >= 0x0300) {
            return false;
        }
    }
    return true;
}

/// Forma compuesta (NFC). "é" puede estar guardada como una letra o como "e"
/// más un acento (así lo hace macOS); comparando en NFC son iguales.
QString toNfc(const QString &text)
{
    return isComposedForSure(text) ? text : text.normalized(QString::NormalizationForm_C);
}

/// Para comparar valores de filtros sin que importen mayúsculas ni acentos:
/// "Vídeo", "video" y "VIDEO" dan lo mismo.
QString plainKey(QStringView text)
{
    const QString decomposed = text.toString().normalized(QString::NormalizationForm_D);
    QString result;
    for (const QChar character : decomposed) {
        if (character.category() != QChar::Mark_NonSpacing) {
            result.append(character.toCaseFolded());
        }
    }
    return result;
}

/// Letra en su forma "sin mayúsculas" para comparar. El caso ASCII, que es
/// casi todo lo que aparece en nombres de archivo, se resuelve sin consultar
/// las tablas de Unicode: esta función se llama millones de veces al filtrar.
inline char16_t foldChar(QChar character)
{
    const char16_t code = character.unicode();
    if (code < 0x80) {
        return (code >= u'A' && code <= u'Z') ? char16_t(code + 32) : code;
    }
    return character.toCaseFolded().unicode();
}

/// El patrón se guarda ya plegado cuando no se distinguen mayúsculas, para
/// plegar solo el nombre al comparar.
QString foldPattern(const QString &pattern, Qt::CaseSensitivity sensitivity)
{
    if (sensitivity == Qt::CaseSensitive) {
        return pattern;
    }
    QString folded = pattern;
    for (QChar &character : folded) {
        character = QChar(foldChar(character));
    }
    return folded;
}

inline bool sameChar(QChar patternChar, QChar nameChar, bool fold)
{
    return patternChar == nameChar || (fold && patternChar.unicode() == foldChar(nameChar));
}

/// ¿Contiene `name` el texto `pattern`? (`pattern` ya plegado si `fold`).
bool containsText(QStringView pattern, QStringView name, bool fold)
{
    const qsizetype patternSize = pattern.size();
    if (patternSize == 0) {
        return true;
    }
    const QChar first = pattern[0];
    for (qsizetype start = 0; start + patternSize <= name.size(); ++start) {
        if (!sameChar(first, name[start], fold)) {
            continue;
        }
        qsizetype offset = 1;
        while (offset < patternSize && sameChar(pattern[offset], name[start + offset], fold)) {
            ++offset;
        }
        if (offset == patternSize) {
            return true;
        }
    }
    return false;
}

/// Comodines sobre el nombre completo: `?` es un carácter y `*` cualquier
/// cantidad. Algoritmo clásico con vuelta atrás al último `*`; no reserva memoria.
bool wildcardMatch(QStringView pattern, QStringView name, bool fold)
{
    qsizetype p = 0;
    qsizetype n = 0;
    qsizetype starPattern = -1;
    qsizetype starName = 0;
    while (n < name.size()) {
        if (p < pattern.size() && pattern[p] == QLatin1Char('*')) {
            starPattern = p++;
            starName = n;
        } else if (p < pattern.size() && (pattern[p] == QLatin1Char('?') || sameChar(pattern[p], name[n], fold))) {
            ++p;
            ++n;
        } else if (starPattern >= 0) {
            p = starPattern + 1;
            n = ++starName;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == QLatin1Char('*')) {
        ++p;
    }
    return p == pattern.size();
}

/// true si esta parte de la consulta necesita leer el archivo.
bool nodeUsesContent(const Query::Node &node)
{
    if (node.kind == Kind::Content) {
        return true;
    }
    for (const NodePtr &child : node.children) {
        if (nodeUsesContent(*child)) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Tipos de archivo
// ---------------------------------------------------------------------------

const QHash<QString, TypeCategory> &categoryNames()
{
    static const QHash<QString, TypeCategory> names{
        {QStringLiteral("imagen"), TypeCategory::Image},     {QStringLiteral("imagenes"), TypeCategory::Image},
        {QStringLiteral("image"), TypeCategory::Image},      {QStringLiteral("images"), TypeCategory::Image},
        {QStringLiteral("foto"), TypeCategory::Image},       {QStringLiteral("fotos"), TypeCategory::Image},
        {QStringLiteral("video"), TypeCategory::Video},      {QStringLiteral("videos"), TypeCategory::Video},
        {QStringLiteral("audio"), TypeCategory::Audio},      {QStringLiteral("musica"), TypeCategory::Audio},
        {QStringLiteral("music"), TypeCategory::Audio},
        {QStringLiteral("documento"), TypeCategory::Document}, {QStringLiteral("documentos"), TypeCategory::Document},
        {QStringLiteral("document"), TypeCategory::Document},  {QStringLiteral("documents"), TypeCategory::Document},
        {QStringLiteral("texto"), TypeCategory::Text},       {QStringLiteral("text"), TypeCategory::Text},
        {QStringLiteral("comprimido"), TypeCategory::Archive}, {QStringLiteral("comprimidos"), TypeCategory::Archive},
        {QStringLiteral("archive"), TypeCategory::Archive},
        {QStringLiteral("codigo"), TypeCategory::Code},      {QStringLiteral("code"), TypeCategory::Code},
        {QStringLiteral("carpeta"), TypeCategory::Folder},   {QStringLiteral("carpetas"), TypeCategory::Folder},
        {QStringLiteral("folder"), TypeCategory::Folder},    {QStringLiteral("directorio"), TypeCategory::Folder},
        {QStringLiteral("archivo"), TypeCategory::File},     {QStringLiteral("archivos"), TypeCategory::File},
        {QStringLiteral("file"), TypeCategory::File},
    };
    return names;
}

QSet<QString> makeSet(const char *spaceSeparated)
{
    QSet<QString> set;
    const QStringList parts = QString::fromLatin1(spaceSeparated).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        set.insert(part);
    }
    return set;
}

const QSet<QString> &extensionsOf(TypeCategory category)
{
    static const QSet<QString> image = makeSet(
        "jpg jpeg png gif bmp webp tif tiff heic heif avif svg ico raw cr2 cr3 nef arw dng orf rw2 psd xcf");
    static const QSet<QString> video = makeSet("mp4 mkv avi mov wmv flv webm m4v mpg mpeg 3gp m2ts mts vob ogv");
    static const QSet<QString> audio = makeSet("mp3 flac wav ogg oga opus m4a aac wma aiff ape mid midi");
    static const QSet<QString> document = makeSet(
        "pdf doc docx odt rtf xls xlsx ods ppt pptx odp epub mobi azw3 djvu");
    static const QSet<QString> text = makeSet("txt md markdown log csv tsv json xml yaml yml ini cfg conf tex rst");
    static const QSet<QString> archive = makeSet("zip rar 7z tar gz bz2 xz zst tgz tbz2 txz iso cab jar");
    static const QSet<QString> code = makeSet(
        "c h cpp hpp cc cxx py js ts java kt rs go rb php cs swift sh bat ps1 html htm css sql lua pl cmake");
    static const QSet<QString> none;
    switch (category) {
    case TypeCategory::Image:    return image;
    case TypeCategory::Video:    return video;
    case TypeCategory::Audio:    return audio;
    case TypeCategory::Document: return document;
    case TypeCategory::Text:     return text;
    case TypeCategory::Archive:  return archive;
    case TypeCategory::Code:     return code;
    case TypeCategory::Folder:
    case TypeCategory::File:     break;
    }
    return none;
}

// ---------------------------------------------------------------------------
// Fechas
// ---------------------------------------------------------------------------

struct Period
{
    qint64 startMs = 0;
    qint64 endMs = 0;   ///< Primer instante que ya no pertenece al periodo.
    bool ok = false;
};

qint64 startOfDayMs(const QDate &date)
{
    return date.startOfDay().toMSecsSinceEpoch();
}

/// "2025" es el año entero, "2025-03" el mes, "2025-03-14" el día.
/// También "hoy"/"today" y "ayer"/"yesterday".
Period parsePeriod(QStringView text, qint64 nowMs)
{
    Period period;
    const QString key = plainKey(text.trimmed());
    const QDate today = QDateTime::fromMSecsSinceEpoch(nowMs).date();
    if (key == QLatin1String("hoy") || key == QLatin1String("today")) {
        period = {startOfDayMs(today), startOfDayMs(today.addDays(1)), true};
        return period;
    }
    if (key == QLatin1String("ayer") || key == QLatin1String("yesterday")) {
        period = {startOfDayMs(today.addDays(-1)), startOfDayMs(today), true};
        return period;
    }
    static const QRegularExpression pattern(QStringLiteral("^(\\d{4})(?:-(\\d{1,2})(?:-(\\d{1,2}))?)?$"));
    const QRegularExpressionMatch match = pattern.match(key);
    if (!match.hasMatch()) {
        return period;
    }
    const int year = match.captured(1).toInt();
    if (match.captured(2).isEmpty()) {
        const QDate first(year, 1, 1);
        if (first.isValid()) {
            period = {startOfDayMs(first), startOfDayMs(first.addYears(1)), true};
        }
        return period;
    }
    const int month = match.captured(2).toInt();
    if (match.captured(3).isEmpty()) {
        const QDate first(year, month, 1);
        if (first.isValid()) {
            period = {startOfDayMs(first), startOfDayMs(first.addMonths(1)), true};
        }
        return period;
    }
    const QDate day(year, month, match.captured(3).toInt());
    if (day.isValid()) {
        period = {startOfDayMs(day), startOfDayMs(day.addDays(1)), true};
    }
    return period;
}

// ---------------------------------------------------------------------------
// Analizador léxico: convierte el texto en piezas
// ---------------------------------------------------------------------------

struct Token
{
    enum class Type { LeftParen, RightParen, And, Or, Not, Term, Filter, End };
    Type type = Type::End;
    QString key;      ///< Filter: nombre del filtro, en minúsculas.
    QString value;    ///< Term o Filter: el texto.
    QString flags;    ///< Solo regex: letras tras la barra final.
    bool quoted = false;
    qsizetype position = 0;
};

bool isFilterKey(const QString &key)
{
    static const QSet<QString> keys{
        QStringLiteral("ext"),  QStringLiteral("size"),   QStringLiteral("modified"), QStringLiteral("created"),
        QStringLiteral("type"), QStringLiteral("tag"),    QStringLiteral("hidden"),   QStringLiteral("content"),
        QStringLiteral("regex"), QStringLiteral("name"),
    };
    return keys.contains(key);
}

class Lexer
{
public:
    explicit Lexer(const QString &text) : m_text(text) {}

    /// Devuelve la lista de piezas o deja un mensaje en `error`.
    QList<Token> run(QString *error)
    {
        QList<Token> tokens;
        while (true) {
            skipSpaces();
            Token token;
            token.position = m_position;
            if (m_position >= m_text.size()) {
                token.type = Token::Type::End;
                tokens.append(token);
                return tokens;
            }
            const QChar character = m_text[m_position];
            if (character == QLatin1Char('(')) {
                token.type = Token::Type::LeftParen;
                ++m_position;
            } else if (character == QLatin1Char(')')) {
                token.type = Token::Type::RightParen;
                ++m_position;
            } else if (character == QLatin1Char('"')) {
                token.type = Token::Type::Term;
                token.quoted = true;
                if (!readQuoted(&token.value, error)) {
                    return {};
                }
            } else if (character == QLatin1Char('-') && m_position + 1 < m_text.size()
                       && !m_text[m_position + 1].isSpace() && startsTerm()) {
                // "-borrador" es lo mismo que "NOT borrador".
                token.type = Token::Type::Not;
                ++m_position;
            } else if (!readWord(&token, error)) {
                return {};
            }
            tokens.append(token);
        }
    }

private:
    void skipSpaces()
    {
        while (m_position < m_text.size() && m_text[m_position].isSpace()) {
            ++m_position;
        }
    }

    /// true si en la posición actual empieza un término (y no continúa uno).
    bool startsTerm() const
    {
        return m_position == 0 || m_text[m_position - 1].isSpace() || m_text[m_position - 1] == QLatin1Char('(');
    }

    bool readQuoted(QString *value, QString *error)
    {
        const qsizetype start = m_position;
        ++m_position;   // comilla de apertura
        while (m_position < m_text.size()) {
            const QChar character = m_text[m_position++];
            if (character == QLatin1Char('\\') && m_position < m_text.size()
                && m_text[m_position] == QLatin1Char('"')) {
                value->append(QLatin1Char('"'));
                ++m_position;
            } else if (character == QLatin1Char('"')) {
                return true;
            } else {
                value->append(character);
            }
        }
        *error = tr("Falta la comilla de cierre (la de apertura está en la posición %1).").arg(start + 1);
        return false;
    }

    bool readRegex(Token *token, QString *error)
    {
        const qsizetype start = m_position;
        if (m_position >= m_text.size() || m_text[m_position] != QLatin1Char('/')) {
            *error = tr("Después de regex: va el patrón entre barras, por ejemplo regex:/^IMG_\\d+/ (posición %1).")
                         .arg(start + 1);
            return false;
        }
        ++m_position;
        while (m_position < m_text.size()) {
            const QChar character = m_text[m_position++];
            if (character == QLatin1Char('\\') && m_position < m_text.size()) {
                // "\/" es una barra dentro del patrón; lo demás se deja tal cual.
                if (m_text[m_position] != QLatin1Char('/')) {
                    token->value.append(character);
                }
                token->value.append(m_text[m_position++]);
            } else if (character == QLatin1Char('/')) {
                while (m_position < m_text.size() && m_text[m_position].isLetter()) {
                    token->flags.append(m_text[m_position++]);
                }
                return true;
            } else {
                token->value.append(character);
            }
        }
        *error = tr("Falta la barra de cierre de la expresión regular (empieza en la posición %1).").arg(start + 1);
        return false;
    }

    bool readWord(Token *token, QString *error)
    {
        const qsizetype start = m_position;
        // ¿Es un filtro "clave:valor"?
        qsizetype colon = start;
        while (colon < m_text.size() && m_text[colon].isLetter()) {
            ++colon;
        }
        if (colon > start && colon < m_text.size() && m_text[colon] == QLatin1Char(':')) {
            const QString key = m_text.mid(start, colon - start).toLower();
            if (isFilterKey(key)) {
                token->type = Token::Type::Filter;
                token->key = key;
                m_position = colon + 1;
                if (key == QLatin1String("regex")) {
                    return readRegex(token, error);
                }
                if (m_position < m_text.size() && m_text[m_position] == QLatin1Char('"')) {
                    token->quoted = true;
                    return readQuoted(&token->value, error);
                }
                token->value = readUntilBreak();
                if (token->value.isEmpty()) {
                    *error = tr("Falta el valor del filtro «%1:» (posición %2).").arg(key).arg(start + 1);
                    return false;
                }
                return true;
            }
        }
        // Palabra corriente. Si no es un filtro conocido, los dos puntos son
        // parte del nombre (en Linux y macOS un archivo puede llevarlos).
        const QString word = readUntilBreak();
        if (word == QLatin1String("AND")) {
            token->type = Token::Type::And;
        } else if (word == QLatin1String("OR")) {
            token->type = Token::Type::Or;
        } else if (word == QLatin1String("NOT")) {
            token->type = Token::Type::Not;
        } else {
            token->type = Token::Type::Term;
            token->value = word;
        }
        return true;
    }

    QString readUntilBreak()
    {
        const qsizetype start = m_position;
        while (m_position < m_text.size() && !m_text[m_position].isSpace() && m_text[m_position] != QLatin1Char('(')
               && m_text[m_position] != QLatin1Char(')')) {
            ++m_position;
        }
        return m_text.mid(start, m_position - start);
    }

    QString m_text;
    qsizetype m_position = 0;
};

// ---------------------------------------------------------------------------
// Analizador sintáctico: arma el árbol respetando NOT > AND > OR
// ---------------------------------------------------------------------------

class Parser
{
public:
    Parser(QList<Token> tokens, const QueryOptions &options, qint64 nowMs)
        : m_tokens(std::move(tokens)),
          m_sensitivity(options.caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive),
          m_termsInContent(options.termsInContent), m_nowMs(nowMs)
    {
    }

    NodePtr run()
    {
        if (peek().type == Token::Type::End) {
            return nullptr;   // consulta vacía
        }
        NodePtr root = parseOr();
        if (root && peek().type != Token::Type::End) {
            fail(peek().type == Token::Type::RightParen
                     ? tr("Sobra un paréntesis de cierre (posición %1).").arg(peek().position + 1)
                     : tr("No se esperaba esto en la posición %1.").arg(peek().position + 1));
        }
        return m_error.isEmpty() ? root : nullptr;
    }

    QString error() const { return m_error; }
    QStringList unavailable() const { return m_unavailable; }

private:
    const Token &peek() const { return m_tokens[m_index]; }
    /// Consume una pieza. La última (End) no se consume nunca: así mirar la
    /// siguiente es siempre válido, también con consultas a medio escribir.
    Token take()
    {
        const Token token = m_tokens[m_index];
        if (token.type != Token::Type::End) {
            ++m_index;
        }
        return token;
    }

    void fail(const QString &message)
    {
        if (m_error.isEmpty()) {
            m_error = message;
        }
    }

    static NodePtr combine(Kind kind, std::vector<NodePtr> children)
    {
        if (children.size() == 1) {
            return children.front();
        }
        // En un AND, lo que obliga a leer el archivo va al final: si el
        // nombre, el tamaño o la fecha ya lo descartan, no se llega a abrir.
        if (kind == Kind::And) {
            std::stable_partition(children.begin(), children.end(), [](const NodePtr &child) {
                return child && !nodeUsesContent(*child);
            });
        }
        auto node = std::make_shared<Query::Node>();
        node->kind = kind;
        node->children = std::move(children);
        return node;
    }

    static bool hasWildcards(const QString &text)
    {
        return text.contains(QLatin1Char('*')) || text.contains(QLatin1Char('?'));
    }

    /// Lo que va entre dos operadores. Varias palabras seguidas sin comillas
    /// se leen juntas: tienen que estar todas (AND) y, si alguna lleva
    /// comodines, también vale que el nombre encaje con la frase tal como se
    /// escribió. Así "walter riso*" encuentra "Walter Riso - Amar.pdf", que
    /// contiene "walter" pero no empieza por "riso".
    NodePtr parseOperand()
    {
        if (m_termsInContent || peek().type != Token::Type::Term || peek().quoted) {
            return parseNot();
        }
        std::vector<NodePtr> words;
        QStringList phrase;
        bool wildcards = false;
        while (peek().type == Token::Type::Term && !peek().quoted) {
            const Token token = take();
            words.push_back(makeNameNode(token.value, false));
            phrase.append(token.value);
            wildcards = wildcards || hasWildcards(token.value);
        }
        const bool several = words.size() > 1;
        NodePtr all = combine(Kind::And, std::move(words));
        if (!several || !wildcards) {
            return all;
        }
        return combine(Kind::Or, {all, makeNameNode(phrase.join(QLatin1Char(' ')), false)});
    }

    NodePtr parseOr()
    {
        std::vector<NodePtr> alternatives;
        alternatives.push_back(parseAnd());
        while (m_error.isEmpty() && peek().type == Token::Type::Or) {
            const Token operatorToken = take();
            if (peek().type == Token::Type::End || peek().type == Token::Type::RightParen) {
                fail(tr("Falta algo después de OR (posición %1).").arg(operatorToken.position + 1));
                break;
            }
            alternatives.push_back(parseAnd());
        }
        return combine(Kind::Or, std::move(alternatives));
    }

    NodePtr parseAnd()
    {
        std::vector<NodePtr> parts;
        parts.push_back(parseOperand());
        while (m_error.isEmpty()) {
            const Token::Type next = peek().type;
            if (next == Token::Type::Or || next == Token::Type::RightParen || next == Token::Type::End) {
                break;
            }
            if (next == Token::Type::And) {
                const Token operatorToken = take();
                const Token::Type after = peek().type;
                if (after == Token::Type::End || after == Token::Type::RightParen || after == Token::Type::Or
                    || after == Token::Type::And) {
                    fail(tr("Falta algo después de AND (posición %1).").arg(operatorToken.position + 1));
                    break;
                }
            }
            parts.push_back(parseOperand());   // un espacio entre términos equivale a AND
        }
        return combine(Kind::And, std::move(parts));
    }

    NodePtr parseNot()
    {
        if (peek().type == Token::Type::Not) {
            const Token operatorToken = take();
            const Token::Type after = peek().type;
            if (after == Token::Type::End || after == Token::Type::RightParen || after == Token::Type::Or
                || after == Token::Type::And) {
                fail(tr("Falta algo después de NOT (posición %1).").arg(operatorToken.position + 1));
                return nullptr;
            }
            auto node = std::make_shared<Query::Node>();
            node->kind = Kind::Not;
            node->children.push_back(parseNot());
            return node;
        }
        return parsePrimary();
    }

    NodePtr parsePrimary()
    {
        const Token token = take();
        switch (token.type) {
        case Token::Type::LeftParen: {
            if (peek().type == Token::Type::RightParen) {
                fail(tr("Los paréntesis de la posición %1 están vacíos.").arg(token.position + 1));
                return nullptr;
            }
            NodePtr inner = parseOr();
            if (peek().type != Token::Type::RightParen) {
                fail(tr("Falta cerrar el paréntesis abierto en la posición %1.").arg(token.position + 1));
                return nullptr;
            }
            take();
            return inner;
        }
        case Token::Type::Term:
            return m_termsInContent ? makeContentNode(token.value, token.quoted)
                                    : makeNameNode(token.value, token.quoted);
        case Token::Type::Filter:
            return makeFilter(token);
        case Token::Type::Or:
        case Token::Type::And:
            fail(tr("Falta algo antes del operador de la posición %1.").arg(token.position + 1));
            return nullptr;
        case Token::Type::RightParen:
            fail(tr("Sobra un paréntesis de cierre (posición %1).").arg(token.position + 1));
            return nullptr;
        case Token::Type::Not:
        case Token::Type::End:
            break;
        }
        fail(tr("La consulta está incompleta."));
        return nullptr;
    }

    /// Un término suelto. Sin comodines basta con que el nombre lo contenga
    /// ("factura" encuentra "mi factura.pdf"); con `*` o `?` tiene que encajar
    /// el nombre entero ("fot*.jpg"). Entre comillas todo es texto literal.
    NodePtr makeNameNode(const QString &text, bool quoted) const
    {
        auto node = std::make_shared<Query::Node>();
        node->text = foldPattern(toNfc(text), m_sensitivity);
        node->caseSensitivity = m_sensitivity;
        node->kind = (!quoted && hasWildcards(text)) ? Kind::Wildcard : Kind::Substring;
        return node;
    }

    /// Texto que tiene que aparecer dentro del archivo. Es siempre "contiene":
    /// los asteriscos de los extremos sobran y se quitan; entre comillas el
    /// texto se toma tal cual.
    NodePtr makeContentNode(const QString &text, bool quoted) const
    {
        QString wanted = toNfc(text);
        if (!quoted) {
            while (wanted.startsWith(QLatin1Char('*'))) {
                wanted.remove(0, 1);
            }
            while (wanted.endsWith(QLatin1Char('*'))) {
                wanted.chop(1);
            }
        }
        auto node = std::make_shared<Query::Node>();
        node->kind = Kind::Content;
        node->text = wanted;
        node->caseSensitivity = m_sensitivity;
        return node;
    }

    NodePtr makeFilter(const Token &token)
    {
        const QString where = tr(" (posición %1)").arg(token.position + 1);
        auto node = std::make_shared<Query::Node>();
        node->caseSensitivity = m_sensitivity;

        if (token.key == QLatin1String("name")) {
            return makeNameNode(token.value, token.quoted);
        }
        if (token.key == QLatin1String("ext")) {
            node->kind = Kind::Extension;
            const QStringList parts = token.value.split(QLatin1Char(','), Qt::SkipEmptyParts);
            for (QString part : parts) {
                part = toNfc(part.trimmed());
                if (part.startsWith(QLatin1Char('.'))) {
                    part.remove(0, 1);
                }
                if (!part.isEmpty()) {
                    node->list.append(QLatin1Char('.') + part);
                }
            }
            if (node->list.isEmpty()) {
                fail(tr("ext: necesita una extensión, por ejemplo ext:pdf o ext:jpg,png") + where);
            }
            return node;
        }
        if (token.key == QLatin1String("size")) {
            node->kind = Kind::Size;
            if (!parseSizeRange(token.value, node.get())) {
                fail(tr("No se entiende el tamaño «%1». Ejemplos: size:>10MB, size:<=500kB, size:1MB..2GB")
                         .arg(token.value)
                     + where);
            }
            return node;
        }
        if (token.key == QLatin1String("modified") || token.key == QLatin1String("created")) {
            node->kind = token.key == QLatin1String("modified") ? Kind::Modified : Kind::Created;
            if (!parseDateRange(token.value, node.get())) {
                fail(tr("No se entiende la fecha «%1». Ejemplos: %2:2025, %2:2025-01..2025-06, %2:>=2024-12-31, %2:hoy")
                         .arg(token.value, token.key)
                     + where);
            }
            return node;
        }
        if (token.key == QLatin1String("type")) {
            node->kind = Kind::Type;
            const auto found = categoryNames().constFind(plainKey(token.value));
            if (found == categoryNames().constEnd()) {
                fail(tr("No se conoce el tipo «%1». Tipos: imagen, video, audio, documento, texto, comprimido, "
                        "código, carpeta, archivo")
                         .arg(token.value)
                     + where);
                return node;
            }
            node->category = found.value();
            return node;
        }
        if (token.key == QLatin1String("hidden")) {
            node->kind = Kind::Hidden;
            const QString value = plainKey(token.value);
            static const QSet<QString> yes{QStringLiteral("yes"), QStringLiteral("si"), QStringLiteral("true"),
                                           QStringLiteral("1")};
            static const QSet<QString> no{QStringLiteral("no"), QStringLiteral("false"), QStringLiteral("0")};
            if (yes.contains(value)) {
                node->flag = true;
            } else if (no.contains(value)) {
                node->flag = false;
            } else {
                fail(tr("hidden: admite sí o no (también yes/no)") + where);
            }
            return node;
        }
        if (token.key == QLatin1String("regex")) {
            node->kind = Kind::Regex;
            QRegularExpression::PatternOptions options = QRegularExpression::UseUnicodePropertiesOption;
            if (m_sensitivity == Qt::CaseInsensitive || token.flags.contains(QLatin1Char('i'))) {
                options |= QRegularExpression::CaseInsensitiveOption;
            }
            node->regex = QRegularExpression(toNfc(token.value), options);
            if (!node->regex.isValid()) {
                fail(tr("La expresión regular no es válida: %1").arg(node->regex.errorString()) + where);
            } else {
                node->regex.optimize();
            }
            return node;
        }
        if (token.key == QLatin1String("content")) {
            return makeContentNode(token.value, token.quoted);
        }
        // tag: existe en la gramática, pero aún no se evalúa.
        node->kind = Kind::Unavailable;
        if (!m_unavailable.contains(token.key)) {
            m_unavailable.append(token.key);
        }
        return node;
    }

    static bool splitComparator(QStringView text, QString *comparator, QStringView *rest)
    {
        static const QStringList comparators{QStringLiteral(">="), QStringLiteral("<="), QStringLiteral(">"),
                                             QStringLiteral("<"), QStringLiteral("=")};
        for (const QString &candidate : comparators) {
            if (text.startsWith(candidate)) {
                *comparator = candidate;
                *rest = text.mid(candidate.size());
                return true;
            }
        }
        *rest = text;
        return false;
    }

    static bool parseSizeRange(const QString &value, Query::Node *node)
    {
        const qsizetype dots = value.indexOf(QLatin1String(".."));
        if (dots >= 0) {
            const QStringView from = QStringView(value).left(dots).trimmed();
            const QStringView to = QStringView(value).mid(dots + 2).trimmed();
            if (from.isEmpty() && to.isEmpty()) {
                return false;
            }
            if (!from.isEmpty()) {
                node->low = parseSize(from);
                if (node->low < 0) {
                    return false;
                }
            }
            if (!to.isEmpty()) {
                node->high = parseSize(to);
                if (node->high < 0) {
                    return false;
                }
            }
            return node->low <= node->high;
        }
        QString comparator;
        QStringView rest;
        splitComparator(value, &comparator, &rest);
        qint64 unit = 1;
        const qint64 bytes = parseSize(rest, &unit);
        if (bytes < 0) {
            return false;
        }
        if (comparator == QLatin1String(">")) {
            node->low = bytes + 1;
        } else if (comparator == QLatin1String(">=")) {
            node->low = bytes;
        } else if (comparator == QLatin1String("<")) {
            node->high = bytes - 1;
        } else if (comparator == QLatin1String("<=")) {
            node->high = bytes;
        } else if (comparator == QLatin1String("=")) {
            node->low = bytes;
            node->high = bytes;
        } else {
            // "size:10MB" a secas: lo que se mostraría redondeado como 10 MB,
            // es decir, de 9,5 MB a menos de 10,5 MB. Nadie busca un archivo
            // de exactamente 10 000 000 bytes.
            node->low = bytes - unit / 2;
            node->high = bytes + unit / 2 - (unit > 1 ? 1 : 0);
        }
        return true;
    }

    bool parseDateRange(const QString &value, Query::Node *node) const
    {
        const qsizetype dots = value.indexOf(QLatin1String(".."));
        if (dots >= 0) {
            const QStringView from = QStringView(value).left(dots).trimmed();
            const QStringView to = QStringView(value).mid(dots + 2).trimmed();
            if (from.isEmpty() && to.isEmpty()) {
                return false;
            }
            if (!from.isEmpty()) {
                const Period period = parsePeriod(from, m_nowMs);
                if (!period.ok) {
                    return false;
                }
                node->low = period.startMs;
            }
            if (!to.isEmpty()) {
                const Period period = parsePeriod(to, m_nowMs);
                if (!period.ok) {
                    return false;
                }
                node->high = period.endMs;   // el periodo final entra entero
            }
            return node->low < node->high;
        }
        QString comparator;
        QStringView rest;
        splitComparator(value, &comparator, &rest);
        const Period period = parsePeriod(rest, m_nowMs);
        if (!period.ok) {
            return false;
        }
        if (comparator == QLatin1String(">")) {
            node->low = period.endMs;
        } else if (comparator == QLatin1String(">=")) {
            node->low = period.startMs;
        } else if (comparator == QLatin1String("<")) {
            node->high = period.startMs;
        } else if (comparator == QLatin1String("<=")) {
            node->high = period.endMs;
        } else {
            node->low = period.startMs;
            node->high = period.endMs;
        }
        return true;
    }

    QList<Token> m_tokens;
    qsizetype m_index = 0;
    Qt::CaseSensitivity m_sensitivity;
    bool m_termsInContent;
    qint64 m_nowMs;
    QString m_error;
    QStringList m_unavailable;
};

// ---------------------------------------------------------------------------
// Evaluación
// ---------------------------------------------------------------------------

bool hasExtension(QStringView name, const QString &dotExtension)
{
    // "foto.JPG" cumple ext:jpg; ".bashrc" no cumple ext:bashrc (no es una
    // extensión, es el nombre entero).
    return name.size() > dotExtension.size() && name.endsWith(dotExtension, Qt::CaseInsensitive);
}

bool evaluate(const Query::Node &node, const Item &item, QStringView name, ContentSource *content)
{
    switch (node.kind) {
    case Kind::And:
        for (const NodePtr &child : node.children) {
            if (!evaluate(*child, item, name, content)) {
                return false;
            }
        }
        return true;
    case Kind::Or:
        for (const NodePtr &child : node.children) {
            if (evaluate(*child, item, name, content)) {
                return true;
            }
        }
        return false;
    case Kind::Not:
        return !evaluate(*node.children.front(), item, name, content);
    case Kind::Substring:
        return containsText(node.text, name, node.caseSensitivity == Qt::CaseInsensitive);
    case Kind::Wildcard:
        return wildcardMatch(node.text, name, node.caseSensitivity == Qt::CaseInsensitive);
    case Kind::Extension:
        for (const QString &extension : node.list) {
            if (hasExtension(name, extension)) {
                return true;
            }
        }
        return false;
    case Kind::Size:
        // El "tamaño" de una carpeta no es el de su contenido: no se compara.
        return !item.isDirectory && item.size >= node.low && item.size <= node.high;
    case Kind::Modified:
        return item.modifiedMs != 0 && item.modifiedMs >= node.low && item.modifiedMs < node.high;
    case Kind::Created:
        return item.createdMs != 0 && item.createdMs >= node.low && item.createdMs < node.high;
    case Kind::Type: {
        if (node.category == TypeCategory::Folder) {
            return item.isDirectory;
        }
        if (item.isDirectory) {
            return false;
        }
        if (node.category == TypeCategory::File) {
            return true;
        }
        const qsizetype dot = name.lastIndexOf(QLatin1Char('.'));
        if (dot <= 0 || dot == name.size() - 1) {
            return false;
        }
        return extensionsOf(node.category).contains(name.mid(dot + 1).toString().toLower());
    }
    case Kind::Hidden:
        return item.isHidden == node.flag;
    case Kind::Regex:
        return node.regex.matchView(name).hasMatch();
    case Kind::Content: {
        if (content == nullptr || item.isDirectory) {
            return false;
        }
        const QString *text = content->text();
        return text != nullptr && text->contains(node.text, node.caseSensitivity);
    }
    case Kind::Unavailable:
        return false;
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// API pública
// ---------------------------------------------------------------------------

Item itemFrom(const fs::Entry &entry)
{
    Item item;
    item.name = entry.name;
    item.size = entry.size;
    item.modifiedMs = entry.modifiedMs;
    item.createdMs = entry.createdMs;
    item.isDirectory = entry.isDirectory;
    item.isHidden = entry.isHidden;
    return item;
}

qint64 parseSize(QStringView text, qint64 *unitBytes)
{
    static const QRegularExpression pattern(QStringLiteral("^([0-9]+)(?:[.,]([0-9]+))?\\s*([a-zA-Z]*)$"));
    const QRegularExpressionMatch match = pattern.matchView(text.trimmed());
    if (!match.hasMatch()) {
        return -1;
    }
    static const QHash<QString, qint64> units{
        {QString(), 1},
        {QStringLiteral("b"), 1},
        {QStringLiteral("k"), 1000LL},
        {QStringLiteral("kb"), 1000LL},
        {QStringLiteral("m"), 1000LL * 1000},
        {QStringLiteral("mb"), 1000LL * 1000},
        {QStringLiteral("g"), 1000LL * 1000 * 1000},
        {QStringLiteral("gb"), 1000LL * 1000 * 1000},
        {QStringLiteral("t"), 1000LL * 1000 * 1000 * 1000},
        {QStringLiteral("tb"), 1000LL * 1000 * 1000 * 1000},
        {QStringLiteral("kib"), 1024LL},
        {QStringLiteral("mib"), 1024LL * 1024},
        {QStringLiteral("gib"), 1024LL * 1024 * 1024},
        {QStringLiteral("tib"), 1024LL * 1024 * 1024 * 1024},
    };
    const auto unit = units.constFind(match.captured(3).toLower());
    if (unit == units.constEnd()) {
        return -1;
    }
    bool ok = false;
    const double number = (match.captured(1) + QLatin1Char('.')
                           + (match.captured(2).isEmpty() ? QStringLiteral("0") : match.captured(2)))
                              .toDouble(&ok);
    if (!ok || number * double(unit.value()) > 9.0e18) {
        return -1;
    }
    if (unitBytes != nullptr) {
        *unitBytes = unit.value();
    }
    return qint64(number * double(unit.value()) + 0.5);
}

Query::Query() = default;

core::Result<Query> Query::parse(const QString &text, const QueryOptions &options)
{
    QString error;
    Lexer lexer(text);
    QList<Token> tokens = lexer.run(&error);
    if (!error.isEmpty()) {
        return core::Error{core::ErrorCode::InvalidName, error, {}, 0};
    }
    const qint64 nowMs = options.nowMs != 0 ? options.nowMs : QDateTime::currentMSecsSinceEpoch();
    Parser parser(std::move(tokens), options, nowMs);
    NodePtr root = parser.run();
    if (!parser.error().isEmpty()) {
        return core::Error{core::ErrorCode::InvalidName, parser.error(), {}, 0};
    }
    Query query;
    query.m_root = std::move(root);
    query.m_unavailable = parser.unavailable();
    query.m_text = text;
    query.m_usesContent = query.m_root != nullptr && nodeUsesContent(*query.m_root);
    return query;
}

bool Query::matches(const Item &item, ContentSource *content) const
{
    if (!m_root) {
        return true;
    }
    // El nombre se compara en forma compuesta (NFC). Casi todos los nombres
    // ya están así con seguridad: entonces se evita copiar la cadena.
    if (isComposedForSure(item.name)) {
        return evaluate(*m_root, item, item.name, content);
    }
    const QString composed = item.name.toString().normalized(QString::NormalizationForm_C);
    return evaluate(*m_root, item, composed, content);
}

} // namespace eu::search
