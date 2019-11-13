BeginPackage["AST`"]

(*
Parsing
*)
ParseString

ParseFile

ParseBytes

ConcreteParseString

ConcreteParseFile

ConcreteParseBytes

ParseLeaf


(*
Tokenizing
*)
TokenizeString

TokenizeFile

TokenizeBytes



(*
Boxes
*)
ConcreteParseBox



(*
ToString
*)
ToInputFormString

ToFullFormString

ToStandardFormBoxes

ToSourceCharacterString



(*
Nodes
*)
ToNode
FromNode

DeclarationName


(*

There are some System symbols that are only created when expressions are parsed:

e.g., HermitianConjugate and ImplicitPlus are System symbols that do not exist until expressions
are parsed:

In[1]:= Names["HermitianConjugate"]
Out[1]= {}
In[2]:= ToExpression["a\\[HermitianConjugate]",InputForm,Hold]//FullForm
Out[2]//FullForm= Hold[ConjugateTranspose[a]]
In[3]:= Names["HermitianConjugate"]
Out[3]= {HermitianConjugate}

In[1]:= Names["ImplicitPlus"]
Out[1]= {}
In[2]:= ToExpression["a\\[ImplicitPlus]b",InputForm,Hold]//FullForm
Out[2]//FullForm= Hold[Plus[a,b]]
In[3]:= Names["ImplicitPlus"]
Out[3]= {ImplicitPlus}

These are not documented symbols, so they are apparently side-effects of parsing.

We want to avoid any confusion about this, so we introduce our own symbols here:
AST`PostfixHermitianConjugate and AST`BinaryImplicitPlus

Some examples of these System symbols that are introduced only after parsing:
HermitianConjugate
ImplicitPlus
InvisiblePrefixScriptBase
InvisiblePostfixScriptBase

*)

(*Token*)
Token


(*Character*)
WLCharacter


(* atom symbols *)
OptionalDefault
PatternBlank
PatternBlankSequence
PatternBlankNullSequence
OptionalDefaultPattern

(* operator symbols *)
PrefixLinearSyntaxBang
PrefixInvisiblePrefixScriptBase

PostfixHermitianConjugate
PostfixInvisiblePostfixScriptBase

BinarySlashSlash
BinaryAt
BinaryAtAtAt

TernaryTilde
TernarySlashColon

Comma


(* group symbols *)
(*List*)

(*Association*)

(*AngleBracket*)

(*Ceiling*)

(*Floor*)

GroupDoubleBracket

GroupSquare

(*BracketingBar*)

(*DoubleBracketingBar*)

GroupParen

GroupLinearSyntaxParen

Shebang

(* option symbols *)
Source
Synthesized

Comment
Metadata

Intra


(* node symbols *)
LeafNode
BoxNode
CodeNode
DirectiveNode

PrefixNode
BinaryNode
TernaryNode
InfixNode
PostfixNode
GroupNode
CallNode
PrefixBinaryNode

StartOfLineNode
StartOfFileNode
BlankNode
BlankSequenceNode
BlankNullSequenceNode
PatternBlankNode
PatternBlankSequenceNode
PatternBlankNullSequenceNode
OptionalDefaultPatternNode

FileNode
StringNode
HoldNode


SyntaxErrorNode
GroupMissingCloserNode
GroupMissingOpenerNode
AbstractSyntaxErrorNode


InternalInvalid

BeginStaticAnalysisIgnore
EndStaticAnalysisIgnore


PackageNode
ContextNode
StaticAnalysisIgnoreNode



(*
Analysis
*)

(* property for SyntaxIssue *)
CodeActions
CodeAction
(*
CodeAction commands
*)
DeleteNode
InsertNode
InsertNodeAfter
ReplaceNode
DeleteText
InsertText
InsertTextAfter
ReplaceText
DeleteTrivia


(*
Used to report f[,] or "\[Alpa]" as an option, e.g. SyntaxIssues -> {SyntaxIssue[], SyntaxIssue[]}
*)
SyntaxIssues
AbstractSyntaxIssues
AbstractFormatIssues
SyntaxIssue
FormatIssue





$Quirks


Begin["`Private`"]

(*
Implementation of Abstract depends on Node (LHS of definitions)
So load Node before Abstract
*)
Needs["AST`Node`"]
Needs["AST`Abstract`"]
Needs["AST`Boxes`"]
Needs["AST`DeclarationName`"]
Needs["AST`Library`"]
Needs["AST`ToString`"]
Needs["AST`Utils`"]
Needs["PacletManager`"]



loadAllFuncs[]



ConcreteParseString::usage = "ConcreteParseString[string] returns a concrete syntax tree by interpreting string as WL input."

Options[ConcreteParseString] = {
	CharacterEncoding -> "UTF8",
	"SourceStyle" -> "LineCol"
}

ConcreteParseString[s_String, h_:Automatic, opts:OptionsPattern[]] :=
	concreteParseString[s, h, opts]


Options[concreteParseString] = Options[ConcreteParseString]

concreteParseString[sIn_String, hIn_, OptionsPattern[]] :=
Catch[
Module[{s, h, res, style, bytes, encoding},

	s = sIn;
	h = hIn;

	style = OptionValue["SourceStyle"];
	encoding = OptionValue[CharacterEncoding];

	If[encoding =!= "UTF8",
		Throw[Failure["OnlyUTF8Supported", <|"CharacterEncoding"->encoding|>]]
	];

	bytes = ToCharacterCode[s, "UTF8"];

	(*
	The <||> will be filled in with Source later
	The # here is { {exprs}, {issues}, {metadata} }
	*)
	If[h === Automatic,
		h = StringNode[String, #[[1]], If[!empty[#[[2]]], <| SyntaxIssues -> #[[2]] |>, <||>]]&
	];

	If[FailureQ[concreteParseBytesFunc],
		Throw[concreteParseBytesFunc]
	];

	$ConcreteParseProgress = 0;
	$ConcreteParseStart = Now;
	$ConcreteParseTime = Quantity[0, "Seconds"];
	$MathLinkTime = Quantity[0, "Seconds"];
	(*
	in the event of an abort, force reload of functions
	This will fix the transient error that can happen when an abort occurs
	and the next use throws LIBRARY_FUNCTION_ERROR
	*)
	CheckAbort[
	res = concreteParseBytesFunc[bytes, style];
	,
	loadAllFuncs[];
	Abort[]
	];

	$MathLinkTime = Now - ($ConcreteParseStart + $ConcreteParseTime);

	If[Head[res] === LibraryFunctionError,
		Throw[Failure["LibraryFunctionError", <|"Result"->res|>]]
	];

	If[FailureQ[res],
		Throw[res]
	];

	h[res]
]]





ParseString::usage = "ParseString[string] returns an abstract syntax tree by interpreting string as WL input. \
Note: If there are multiple expressions in string, then only the last expression is returned. \
ParseString[string, h] wraps the output with h and allows multiple expressions to be returned. \
This is similar to how ToExpression operates."

Options[ParseString] = {
	"SourceStyle" -> "LineCol"
}

(*
may return:
a node
or Null if input was an empty string
or something FailureQ if e.g., no permission to run wl-ast
*)
ParseString[s_String, h_:Automatic, opts:OptionsPattern[]] :=
Catch[
Module[{cst, ast, agg},
	
	cst = ConcreteParseString[s, h, opts];

	If[FailureQ[cst],
		Throw[cst]
	];

	agg = Aggregate[cst];

	ast = Abstract[agg];

	ast
]]




ConcreteParseFile::usage = "ConcreteParseFile[file] returns a concrete syntax tree by interpreting file as WL input."

Options[ConcreteParseFile] = {
	CharacterEncoding -> "UTF8",
	"SourceStyle" -> "LineCol"
}

(*
ConcreteParseFile[file_String] returns a FileNode AST or a Failure object
*)
ConcreteParseFile[file_String | File[file_String], h_:Automatic, opts:OptionsPattern[]] :=
	concreteParseFile[file, h, opts]


Options[concreteParseFile] = Options[ConcreteParseFile]

concreteParseFile[file_String, hIn_, OptionsPattern[]] :=
Catch[
Module[{h, encoding, full, res, data, start, end, children,
	style},

	h = hIn;

	encoding = OptionValue[CharacterEncoding];
	style = OptionValue["SourceStyle"];

	(*
	The <||> will be filled in with Source later
	The # here is { {exprs}, {issues}, {metadata} }
	*)
	If[h === Automatic,
		h = FileNode[File, #[[1]], If[!empty[#[[2]]], <| SyntaxIssues -> #[[2]] |>, <||>]]&
	];

	If[encoding =!= "UTF8",
		Throw[Failure["OnlyUTF8Supported", <|"CharacterEncoding"->encoding|>]]
	];


	(*
	We want to expand anything like ~ before passing to external process

	FindFile does a better job than AbsoluteFileName because it can handle things like "Foo`" also

	FindFile also fails if in sandbox mode
	*)
	full = FindFile[file];
	If[FailureQ[full],
		Throw[Failure["FindFileFailed", <|"FileName"->file|>]]
	];

	If[FailureQ[concreteParseFileFunc],
		Throw[concreteParseFileFunc]
	];

	$ConcreteParseProgress = 0;
	$ConcreteParseStart = Now;
	$ConcreteParseTime = Quantity[0, "Seconds"];
	$MathLinkTime = Quantity[0, "Seconds"];
	CheckAbort[
	res = concreteParseFileFunc[full, style];
	,
	loadAllFuncs[];
	Abort[]
	];

	$MathLinkTime = Now - ($ConcreteParseStart + $ConcreteParseTime);

	If[Head[res] === LibraryFunctionError,
		Throw[Failure["LibraryFunctionError", <|"Result"->res|>]]
	];

	If[FailureQ[res],
		If[res === $Failed,
			Throw[res]
		];
		res = Failure[res[[1]], Join[res[[2]], <|"FileName"->full|>]];
		Throw[res]
	];

	res = h[res];

	(*
	Fill in Source for FileNode now
	*)
	If[hIn === Automatic,
		children = res[[2]];
		(* only fill in if there are actually children nodes to grab *)
		If[children =!= {},
			start = First[children][[3]][Source][[1]];
			end = Last[children][[3]][Source][[2]];
			data = res[[3]];
			AssociateTo[data, Source -> {start, end}];
			res[[3]] = data;
		];
	];

	res
]]




ParseFile::usage = "ParseFile[file] returns an abstract syntax tree by interpreting file as WL input."

Options[ParseFile] = {
	CharacterEncoding -> "UTF8",
	"SourceStyle" -> "LineCol"
}

ParseFile[file_String | File[file_String], h_:Automatic, opts:OptionsPattern[]] :=
Catch[
Module[{cst, ast, agg},

	cst = ConcreteParseFile[file, h, opts];

	If[FailureQ[cst],
		Throw[cst]
	];

	agg = Aggregate[cst];

	ast = Abstract[agg];

	ast
]]





ConcreteParseBytes::usage = "ConcreteParseBytes[bytes] returns a concrete syntax tree by interpreting bytes as WL input."

Options[ConcreteParseBytes] = {
	CharacterEncoding -> "UTF8",
	"SourceStyle" -> "LineCol"
}

(*
ConcreteParseBytes[bytes_List] returns a FileNode AST or a Failure object
*)
ConcreteParseBytes[bytes_List, h_:Automatic, opts:OptionsPattern[]] :=
	concreteParseBytes[bytes, h, opts]


Options[concreteParseBytes] = Options[ConcreteParseBytes]

concreteParseBytes[bytes_List, hIn_, OptionsPattern[]] :=
Catch[
Module[{h, encoding, res, data, start, end, children,
	style},

	h = hIn;

	encoding = OptionValue[CharacterEncoding];
	style = OptionValue["SourceStyle"];

	(*
	The <||> will be filled in with Source later
	The # here is { {exprs}, {issues}, {metadata} }
	*)
	If[h === Automatic,
		h = FileNode[File, #[[1]], If[!empty[#[[2]]], <| SyntaxIssues -> #[[2]] |>, <||>]]&
	];

	If[encoding =!= "UTF8",
		Throw[Failure["OnlyUTF8Supported", <|"CharacterEncoding"->encoding|>]]
	];

	If[FailureQ[concreteParseBytesFunc],
		Throw[concreteParseBytesFunc]
	];

	$ConcreteParseProgress = 0;
	$ConcreteParseStart = Now;
	$ConcreteParseTime = Quantity[0, "Seconds"];
	$MathLinkTime = Quantity[0, "Seconds"];
	CheckAbort[
	res = concreteParseBytesFunc[bytes, style];
	,
	loadAllFuncs[];
	Abort[]
	];

	$MathLinkTime = Now - ($ConcreteParseStart + $ConcreteParseTime);

	If[Head[res] === LibraryFunctionError,
		Throw[Failure["LibraryFunctionError", <|"Result"->res|>]]
	];

	If[FailureQ[res],
		Throw[res]
	];

	res = h[res];

	(*
	Fill in Source for FileNode now
	*)
	If[hIn === Automatic,
		children = res[[2]];
		(* only fill in if there are actually children nodes to grab *)
		If[children =!= {},
			start = First[children][[3]][Source][[1]];
			end = Last[children][[3]][Source][[2]];
			data = res[[3]];
			AssociateTo[data, Source -> {start, end}];
			res[[3]] = data;
		];
	];

	res
]]




ParseBytes::usage = "ParseBytes[bytes] returns an abstract syntax tree by interpreting bytes as WL input."

Options[ParseBytes] = {
	CharacterEncoding -> "UTF8",
	"SourceStyle" -> "LineCol"
}

ParseBytes[bytes_List, h_:Automatic, opts:OptionsPattern[]] :=
Catch[
Module[{cst, ast, agg},

	cst = ConcreteParseBytes[bytes, h, opts];

	If[FailureQ[cst],
		Throw[cst]
	];

	agg = Aggregate[cst];

	ast = Abstract[agg];

	ast
]]




TokenizeString::usage = "TokenizeString[string] returns a list of tokens by interpreting string as WL input."

Options[TokenizeString] = {
	CharacterEncoding -> "UTF8",
	"SourceStyle" -> "LineCol"
}

TokenizeString[s_String] :=
	tokenizeString[s]


Options[tokenizeString] = Options[TokenizeString]

tokenizeString[sIn_String, OptionsPattern[]] :=
Catch[
Module[{s, res, style, bytes, encoding},

	s = sIn;

	encoding = OptionValue[CharacterEncoding];
	style = OptionValue["SourceStyle"];

	If[encoding =!= "UTF8",
		Throw[Failure["OnlyUTF8Supported", <|"CharacterEncoding"->encoding|>]]
	];

	bytes = ToCharacterCode[s, "UTF8"];

	If[FailureQ[tokenizeBytesFunc],
		Throw[tokenizeBytesFunc]
	];

	$ConcreteParseProgress = 0;
	$ConcreteParseStart = Now;
	$ConcreteParseTime = Quantity[0, "Seconds"];
	$MathLinkTime = Quantity[0, "Seconds"];
	CheckAbort[
	res = tokenizeBytesFunc[bytes, style];
	,
	loadAllFuncs[];
	Abort[]
	];

	$MathLinkTime = Now - ($ConcreteParseStart + $ConcreteParseTime);

	If[Head[res] === LibraryFunctionError,
		Throw[Failure["LibraryFunctionError", <|"Result"->res|>]]
	];

	If[FailureQ[res],
		Throw[res]
	];

	res
]]







TokenizeFile::usage = "TokenizeFile[file] returns a list of tokens by interpreting file as WL input."

Options[TokenizeFile] = {
	CharacterEncoding -> "UTF8",
	"SourceStyle" -> "LineCol"
}

TokenizeFile[s_String | File[s_String], opts:OptionsPattern[]] :=
	tokenizeFile[s, opts]




Options[tokenizeFile] = Options[TokenizeFile]

tokenizeFile[file_String, OptionsPattern[]] :=
Catch[
Module[{encoding, res, style, full},

	encoding = OptionValue[CharacterEncoding];
	style = OptionValue["SourceStyle"];

	If[encoding =!= "UTF8",
		Throw[Failure["OnlyUTF8Supported", <|"CharacterEncoding"->encoding|>]]
	];

	full = FindFile[file];
	If[FailureQ[full],
		Throw[Failure["FindFileFailed", <|"FileName"->file|>]]
	];

	If[FailureQ[tokenizeFileFunc],
		Throw[tokenizeFileFunc]
	];

	$ConcreteParseProgress = 0;
	$ConcreteParseStart = Now;
	$ConcreteParseTime = Quantity[0, "Seconds"];
	$MathLinkTime = Quantity[0, "Seconds"];
	CheckAbort[
	res = tokenizeFileFunc[full, style];
	,
	loadAllFuncs[];
	Abort[]
	];

	$MathLinkTime = Now - ($ConcreteParseStart + $ConcreteParseTime);

	If[Head[res] === LibraryFunctionError,
		Throw[Failure["LibraryFunctionError", <|"Result"->res|>]]
	];

	If[FailureQ[res],
		Throw[res]
	];

	res
]]






TokenizeBytes::usage = "TokenizeBytes[bytes] returns a list of tokens by interpreting bytes as WL input."

Options[TokenizeBytes] = {
	CharacterEncoding -> "UTF8",
	"SourceStyle" -> "LineCol"
}

TokenizeBytes[bytes_List, opts:OptionsPattern[]] :=
	tokenizeBytes[bytes, opts]




Options[tokenizeBytes] = Options[TokenizeBytes]

tokenizeBytes[bytes_List, OptionsPattern[]] :=
Catch[
Module[{encoding, res, style},

	encoding = OptionValue[CharacterEncoding];
	style = OptionValue["SourceStyle"];

	If[encoding =!= "UTF8",
		Throw[Failure["OnlyUTF8Supported", <|"CharacterEncoding"->encoding|>]]
	];

	If[FailureQ[tokenizeBytesFunc],
		Throw[tokenizeBytesFunc]
	];

	$ConcreteParseProgress = 0;
	$ConcreteParseStart = Now;
	$ConcreteParseTime = Quantity[0, "Seconds"];
	$MathLinkTime = Quantity[0, "Seconds"];
	CheckAbort[
	res = tokenizeBytesFunc[bytes, style];
	,
	loadAllFuncs[];
	Abort[]
	];

	$MathLinkTime = Now - ($ConcreteParseStart + $ConcreteParseTime);

	If[Head[res] === LibraryFunctionError,
		Throw[Failure["LibraryFunctionError", <|"Result"->res|>]]
	];

	If[FailureQ[res],
		Throw[res]
	];

	res
]]







ParseLeaf::usage = "ParseLeaf[str] returns a LeafNode by interpreting str as a leaf."

Options[ParseLeaf] = {
	"SourceStyle" -> "LineCol",
	"StringifyNextTokenSymbol" -> False,
	"StringifyNextTokenFile" -> False
}

ParseLeaf[str_String, opts:OptionsPattern[]] :=
	parseLeaf[str, opts]


Options[parseLeaf] = Options[ParseLeaf]

parseLeaf[strIn_String, OptionsPattern[]] :=
Catch[
Module[{str, res, leaf, data, exprs, issues, style, stringifyNextTokenSymbol, stringifyNextTokenFile},

	str = strIn;

	style = OptionValue["SourceStyle"];
	stringifyNextTokenSymbol = OptionValue["StringifyNextTokenSymbol"];
	stringifyNextTokenFile = OptionValue["StringifyNextTokenFile"];

	If[FailureQ[parseLeafFunc],
		Throw[parseLeafFunc]
	];

	$ConcreteParseProgress = 0;
	$ConcreteParseStart = Now;
	$ConcreteParseTime = Quantity[0, "Seconds"];
	$MathLinkTime = Quantity[0, "Seconds"];
	(*
	in the event of an abort, force reload of functions
	This will fix the transient error that can happen when an abort occurs
	and the next use throws LIBRARY_FUNCTION_ERROR
	*)
	CheckAbort[
	res = parseLeafFunc[str, style, stringifyNextTokenSymbol, stringifyNextTokenFile];
	,
	loadAllFuncs[];
	Abort[]
	];

	$MathLinkTime = Now - ($ConcreteParseStart + $ConcreteParseTime);

	If[Head[res] === LibraryFunctionError,
		Throw[Failure["LibraryFunctionError", <|"Result"->res|>]]
	];

	If[FailureQ[res],
		Throw[res]
	];

	exprs = res[[1]];
	issues = res[[2]];

	leaf = exprs[[1]];

	If[!empty[issues],
		data = leaf[[3]];
		data[SyntaxIssues] = issues;
		leaf[[3]] = data;
	];

	leaf
]]




setupQuirks[] :=
Module[{},
	
	$Quirks = <||>;

	(*
	Setup "FlattenTimes" quirk

	In non-Prototype builds:
		a / b / c is parsed as Times[a, Power[b, -1], Power[c, -1]]
		-a / b is parsed as Times[-1, a, Power[b, -1]]

	In Prototype builds:
		a / b / c is parsed as Times[Times[a, Power[b, -1]], Power[c, -1]]
		-a / b is parsed as Times[Times[-1, a], Power[b, -1]]
	This is considered the correct behavior going into the future.

	This is setup on bugfix/139531_et_al branch
	Related bugs: 139531
	*)
	If[!Internal`$PrototypeBuild,
		$Quirks["FlattenTimes"] = True
	];

]



setupQuirks[]



End[]

EndPackage[]
