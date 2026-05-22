//go:build android

package main

import (
	"fmt"
	"strings"
)

func parseJNISignature(sig string) string {
	if sig == "" {
		return ""
	}

	if sig[0] != '(' {
		t, _, err := parseJNITypePlain(sig, 0)
		if err != nil {
			return sig
		}
		return f.formatType(t)
	}

	idx := 1
	params := make([]string, 0, 4)
	for idx < len(sig) && sig[idx] != ')' {
		param, next, err := parseJNITypePlain(sig, idx)
		if err != nil {
			return sig
		}
		params = append(params, f.formatType(param))
		idx = next
	}

	if idx >= len(sig) || sig[idx] != ')' {
		return sig
	}

	ret, _, err := parseJNITypePlain(sig, idx+1)
	if err != nil {
		return sig
	}

	return f.colorize(ansiBlue, "(") + strings.Join(params, f.colorize(ansiGray, ", ")) + f.colorize(ansiBlue, "): ") + f.formatType(ret)
}

func parseJNITypePlain(sig string, idx int) (string, int, error) {
	if idx >= len(sig) {
		return "", idx, fmt.Errorf("unexpected end of signature")
	}

	switch sig[idx] {
	case 'V':
		return "void", idx + 1, nil
	case 'Z':
		return "boolean", idx + 1, nil
	case 'B':
		return "byte", idx + 1, nil
	case 'C':
		return "char", idx + 1, nil
	case 'S':
		return "short", idx + 1, nil
	case 'I':
		return "int", idx + 1, nil
	case 'J':
		return "long", idx + 1, nil
	case 'F':
		return "float", idx + 1, nil
	case 'D':
		return "double", idx + 1, nil
	case '[':
		inner, next, err := parseJNITypePlain(sig, idx+1)
		if err != nil {
			return "", idx, err
		}
		return inner + "[]", next, nil
	case 'L':
		end := strings.IndexByte(sig[idx:], ';')
		if end < 0 {
			return "", idx, fmt.Errorf("unterminated object type")
		}
		name := sig[idx+1 : idx+end]
		name = strings.ReplaceAll(name, "/", ".")
		return name, idx + end + 1, nil
	default:
		return "", idx, fmt.Errorf("unknown type %q", sig[idx])
	}
}
