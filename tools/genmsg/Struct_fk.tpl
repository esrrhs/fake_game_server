
{{range .Enums}}    
-- {{.Comment}} Start{{$EName := .Name}}{{$EComment := .Comment}}
{{range .Members}} 
-- [{{$EName}} {{$EComment}}]    {{.Comment}}  
const {{.Name}} = {{.Type}}
{{end}}  
-- {{.Comment}} End
{{end}}  
