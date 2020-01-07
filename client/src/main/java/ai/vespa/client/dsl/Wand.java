// Copyright 2019 Oath Inc. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.
package ai.vespa.client.dsl;

import java.util.List;
import java.util.Map;

public class Wand extends QueryChain {

    private String fieldName;
    private Annotation annotation;
    private Object value;


    Wand(String fieldName, Map<String, Integer> weightedSet) {
        this.fieldName = fieldName;
        this.value = weightedSet;
        this.nonEmpty = true;
    }

    Wand(String fieldName, List<List<Integer>> numeric) {
        boolean invalid = numeric.stream().anyMatch(range -> range.size() != 2);
        if (invalid) {
            throw new IllegalArgumentException("incorrect range format");
        }

        this.fieldName = fieldName;
        this.value = numeric;
        this.nonEmpty = true;
    }

    public Wand annotate(Annotation annotation) {
        this.annotation = annotation;
        return this;
    }

    @Override
    public Select getSelect() {
        return sources.select;
    }

    @Override
    boolean hasPositiveSearchField(String fieldName) {
        // TODO: implementation
        throw new UnsupportedOperationException("method not implemented");
    }

    @Override
    boolean hasPositiveSearchField(String fieldName, Object value) {
        // TODO: implementation
        throw new UnsupportedOperationException("method not implemented");
    }

    @Override
    boolean hasNegativeSearchField(String fieldName) {
        // TODO: implementation
        throw new UnsupportedOperationException("method not implemented");
    }

    @Override
    boolean hasNegativeSearchField(String fieldName, Object value) {
        // TODO: implementation
        throw new UnsupportedOperationException("method not implemented");
    }

    @Override
    public String toString() {
        boolean hasAnnotation = A.hasAnnotation(annotation);
        String s = String.format("wand(%s, %s)", fieldName, Q.gson.toJson(value));
        return hasAnnotation ? String.format("([%s]%s)", annotation, s) : s;
    }
}
