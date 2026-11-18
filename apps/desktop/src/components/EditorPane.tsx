import { useEffect, useRef } from "react";
import { basicSetup } from "codemirror";
import { EditorState, Prec } from "@codemirror/state";
import { EditorView, keymap } from "@codemirror/view";
import { HighlightStyle, StreamLanguage, syntaxHighlighting } from "@codemirror/language";
import { lua } from "@codemirror/legacy-modes/mode/lua";
import { tags } from "@lezer/highlight";

const inkTheme = EditorView.theme(
  {
    "&": {
      height: "100%",
      color: "var(--text)",
      backgroundColor: "transparent",
      fontSize: "13px",
    },
    ".cm-scroller": {
      fontFamily: "var(--font-mono)",
      lineHeight: "1.65",
      overflow: "auto",
    },
    ".cm-content": {
      padding: "16px 0 48px",
      caretColor: "var(--ember)",
    },
    ".cm-line": {
      padding: "0 24px 0 10px",
    },
    ".cm-gutters": {
      backgroundColor: "transparent",
      color: "#4e5a64",
      border: "none",
      paddingLeft: "8px",
    },
    ".cm-lineNumbers .cm-gutterElement": {
      padding: "0 12px 0 6px",
      minWidth: "34px",
    },
    ".cm-activeLine": {
      backgroundColor: "rgba(255, 255, 255, 0.022)",
    },
    ".cm-activeLineGutter": {
      backgroundColor: "transparent",
      color: "#8c97a1",
    },
    "&.cm-focused .cm-selectionBackground, .cm-selectionBackground, ::selection": {
      backgroundColor: "rgba(240, 106, 59, 0.20) !important",
    },
    ".cm-cursor, .cm-dropCursor": {
      borderLeftColor: "var(--ember)",
    },
    ".cm-matchingBracket, &.cm-focused .cm-matchingBracket": {
      backgroundColor: "rgba(240, 106, 59, 0.18)",
      outline: "none",
    },
    ".cm-panels": {
      backgroundColor: "var(--panel)",
      color: "var(--text)",
    },
    ".cm-searchMatch": {
      backgroundColor: "rgba(232, 180, 90, 0.18)",
      outline: "1px solid rgba(232, 180, 90, 0.35)",
    },
    ".cm-tooltip": {
      backgroundColor: "var(--panel)",
      border: "1px solid var(--line)",
      color: "var(--text)",
    },
  },
  { dark: true },
);

const luaHighlight = HighlightStyle.define([
  { tag: tags.comment, color: "#5c6974", fontStyle: "italic" },
  { tag: [tags.keyword, tags.modifier], color: "#e8895f" },
  { tag: [tags.string, tags.special(tags.string)], color: "#8fbf9f" },
  { tag: [tags.number, tags.bool, tags.null], color: "#e8b45a" },
  { tag: [tags.function(tags.variableName), tags.function(tags.propertyName)], color: "#d7dee4" },
  { tag: [tags.typeName, tags.className], color: "#7fb8d8" },
  { tag: [tags.operator, tags.punctuation], color: "#a7b2bc" },
  { tag: tags.propertyName, color: "#c9d2d9" },
  { tag: tags.standard(tags.variableName), color: "#7fb8d8" },
  { tag: tags.variableName, color: "#e4e8eb" },
]);

interface EditorPaneProps {
  docId: string;
  value: string;
  wrap: boolean;
  onChange: (source: string) => void;
  onRun: () => void;
}

export function EditorPane({ docId, value, wrap, onChange, onRun }: EditorPaneProps) {
  const host = useRef<HTMLDivElement>(null);
  const onChangeRef = useRef(onChange);
  onChangeRef.current = onChange;
  const onRunRef = useRef(onRun);
  onRunRef.current = onRun;

  useEffect(() => {
    const parent = host.current;
    if (!parent) return;

    const view = new EditorView({
      state: EditorState.create({
        doc: value,
        extensions: [
          basicSetup,
          StreamLanguage.define(lua),
          inkTheme,
          syntaxHighlighting(luaHighlight),
          EditorView.updateListener.of((update) => {
            if (update.docChanged) {
              onChangeRef.current(update.state.doc.toString());
            }
          }),
          Prec.highest(
            keymap.of([
              {
                key: "Mod-Enter",
                preventDefault: true,
                run: () => {
                  onRunRef.current();
                  return true;
                },
              },
              {
                key: "F5",
                preventDefault: true,
                run: () => {
                  onRunRef.current();
                  return true;
                },
              },
            ]),
          ),
          ...(wrap ? [EditorView.lineWrapping] : []),
        ],
      }),
      parent,
    });

    view.focus();

    return () => {
      view.destroy();
    };
    // Remount per document and when wrapping changes. Live value changes come
    // from the editor itself, so `value` is intentionally read once.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [docId, wrap]);

  return <div className="z-editor" ref={host} />;
}
